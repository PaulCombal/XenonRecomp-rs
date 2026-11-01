#include "rust_cfg.h"

#include <algorithm>
#include <cassert>
#include <fmt/core.h>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

// Turns a control flow graph into nested labeled blocks and loops, in two steps.
//
// 1. Making the graph reducible. A loop with two entries has no place for a
//    `loop` to start: whichever block is the header, the other entry would have
//    to jump into the middle of it. MSVC's output has a few of these (23 of
//    56,677 functions in MW2). Each one gets a dispatcher: a new block that every
//    edge into the loop's entries goes through, having first set `__state` to
//    say which entry it meant. The dispatcher is the loop's only header, which
//    leaves a graph of one header, whatever else was wrong inside it being
//    handled the same way one level down. Only those edges pay for the switch;
//    the rest of the function stays as it was. LLVM's FixIrreducible does this.
//
// 2. Laying out the reducible graph, after Norman Ramsey, "Beyond Relooper"
//    (ICFP 2022). Walking the dominator tree places each block once, inside the
//    block that dominates it:
//
//    - A loop header wraps everything it dominates in `'l: loop { .. }`, and a
//      back edge is `continue 'l`.
//    - A join (two or more forward edges) follows a labeled block that encloses
//      the code that reaches it: `'b: { .. break 'b; .. } join`. The joins a
//      block dominates are nested by reverse postorder, so that each one's
//      label encloses every edge to it.
//    - A block with one forward edge in is simply placed at that edge.
//
//    Every path through a block ends in break, continue or return, so nothing
//    falls off the end of a loop body or a labeled block.

namespace rustcfg
{
    namespace
    {
        std::vector<int> ReversePostorder(const std::vector<Block>& blocks)
        {
            enum : uint8_t { Unseen, Seen };
            std::vector<uint8_t> state(blocks.size(), Unseen);
            std::vector<int> post;
            std::vector<std::pair<int, size_t>> stack{ { 0, 0 } };
            state[0] = Seen;

            while (!stack.empty())
            {
                auto& [node, next] = stack.back();
                if (next < blocks[node].succ.size())
                {
                    const int s = blocks[node].succ[next++];
                    if (s != RETURN && state[s] == Unseen)
                    {
                        state[s] = Seen;
                        stack.emplace_back(s, 0);
                    }
                }
                else
                {
                    post.push_back(node);
                    stack.pop_back();
                }
            }

            std::reverse(post.begin(), post.end());
            return post;
        }

        // Keeps what the entry reaches, block 0 staying first.
        void DropUnreachable(std::vector<Block>& blocks)
        {
            const std::vector<int> order = ReversePostorder(blocks);
            if (order.size() == blocks.size())
                return;

            std::vector<int> remap(blocks.size(), RETURN);
            std::vector<int> kept = order;
            std::sort(kept.begin(), kept.end());
            for (size_t i = 0; i < kept.size(); i++)
                remap[kept[i]] = int(i);

            std::vector<Block> result;
            result.reserve(kept.size());
            for (int old : kept)
            {
                Block block = std::move(blocks[old]);
                for (int& s : block.succ)
                {
                    if (s != RETURN)
                        s = remap[s];
                }
                result.push_back(std::move(block));
            }
            blocks = std::move(result);
        }

        struct Dominators
        {
            std::vector<int> order;  // reverse postorder
            std::vector<int> number; // a block's place in order, -1 if unreachable
            std::vector<int> idom;

            bool Dominates(int a, int b) const
            {
                while (number[b] > number[a])
                    b = idom[b];
                return a == b;
            }
        };

        // Cooper, Harvey and Kennedy, "A Simple, Fast Dominance Algorithm".
        Dominators ComputeDominators(const std::vector<Block>& blocks)
        {
            Dominators d;
            d.order = ReversePostorder(blocks);
            d.number.assign(blocks.size(), -1);
            for (size_t i = 0; i < d.order.size(); i++)
                d.number[d.order[i]] = int(i);

            std::vector<std::vector<int>> preds(blocks.size());
            for (int n : d.order)
            {
                for (int s : blocks[n].succ)
                {
                    if (s != RETURN)
                        preds[s].push_back(n);
                }
            }

            d.idom.assign(blocks.size(), -1);
            d.idom[0] = 0;

            auto intersect = [&](int a, int b)
                {
                    while (a != b)
                    {
                        while (d.number[a] > d.number[b]) a = d.idom[a];
                        while (d.number[b] > d.number[a]) b = d.idom[b];
                    }
                    return a;
                };

            for (bool changed = true; changed;)
            {
                changed = false;
                for (size_t i = 1; i < d.order.size(); i++)
                {
                    const int n = d.order[i];
                    int idom = -1;
                    for (int p : preds[n])
                    {
                        if (d.idom[p] != -1)
                            idom = idom == -1 ? p : intersect(p, idom);
                    }
                    if (d.idom[n] != idom)
                    {
                        d.idom[n] = idom;
                        changed = true;
                    }
                }
            }

            return d;
        }

        bool IsReducible(const std::vector<Block>& blocks, const Dominators& d)
        {
            for (int n : d.order)
            {
                for (int s : blocks[n].succ)
                {
                    // Every retreating edge must be a back edge: a graph is
                    // reducible exactly when that holds, for any depth-first order.
                    if (s != RETURN && d.number[s] <= d.number[n] && !d.Dominates(s, n))
                        return false;
                }
            }
            return true;
        }

        // Tarjan, on the graph induced by the blocks in `region`.
        std::vector<std::vector<int>> StronglyConnected(const std::vector<Block>& blocks, const std::set<int>& region)
        {
            std::map<int, int> index, low;
            std::set<int> onStack;
            std::vector<int> stack;
            std::vector<std::vector<int>> result;
            int counter = 0;

            std::function<void(int)> visit = [&](int n)
                {
                    index[n] = low[n] = counter++;
                    stack.push_back(n);
                    onStack.insert(n);

                    for (int s : blocks[n].succ)
                    {
                        if (s == RETURN || region.count(s) == 0)
                            continue;

                        if (index.count(s) == 0)
                        {
                            visit(s);
                            low[n] = std::min(low[n], low[s]);
                        }
                        else if (onStack.count(s) != 0)
                        {
                            low[n] = std::min(low[n], index[s]);
                        }
                    }

                    if (low[n] == index[n])
                    {
                        std::vector<int> component;
                        int m;
                        do
                        {
                            m = stack.back();
                            stack.pop_back();
                            onStack.erase(m);
                            component.push_back(m);
                        } while (m != n);
                        result.push_back(std::move(component));
                    }
                };

            for (int n : region)
            {
                if (index.count(n) == 0)
                    visit(n);
            }
            return result;
        }

        // Step 1. Gives every cycle within `region` one entry, innermost last.
        void MakeReducible(std::vector<Block>& blocks, const std::set<int>& region, uint32_t& nextId, Stats& stats)
        {
            for (const std::vector<int>& component : StronglyConnected(blocks, region))
            {
                const std::set<int> members(component.begin(), component.end());

                if (component.size() == 1)
                {
                    const Block& only = blocks[component[0]];
                    if (std::find(only.succ.begin(), only.succ.end(), component[0]) == only.succ.end())
                        continue; // not a cycle
                }

                // The entries: blocks of the cycle with an edge from outside it.
                std::set<int> entries;
                if (members.count(0) != 0)
                    entries.insert(0); // the function's own entry is one
                for (size_t n = 0; n < blocks.size(); n++)
                {
                    if (members.count(int(n)) != 0)
                        continue;
                    for (int s : blocks[n].succ)
                    {
                        if (s != RETURN && members.count(s) != 0)
                            entries.insert(s);
                    }
                }

                std::set<int> inner = members;
                if (entries.size() == 1)
                {
                    inner.erase(*entries.begin());
                }
                else
                {
                    // Entry 0 must stay block 0 and has no edge to redirect from
                    // outside, so a cycle holding it starts at a new entry block.
                    // That cannot happen for the function's entry unless it is
                    // also one of the cycle's several entries, and every other
                    // block is reached from it: it dominates the rest and the
                    // component has that one entry. Hence:
                    assert(entries.count(0) == 0);

                    const int dispatcher = int(blocks.size());
                    {
                        Block block;
                        block.id = nextId++;
                        block.term = Term::Switch;
                        block.cond = "__state";
                        uint32_t value = 0;
                        for (int entry : entries)
                        {
                            block.values.push_back(value++);
                            block.succ.push_back(entry);
                        }
                        blocks.push_back(std::move(block));
                    }

                    // Every edge to an entry, from inside the cycle or out, sets
                    // the state and goes to the dispatcher.
                    const std::vector<int> entryList(entries.begin(), entries.end());
                    const size_t edgeCount = blocks.size();
                    for (size_t n = 0; n < edgeCount; n++)
                    {
                        if (int(n) == dispatcher)
                            continue;
                        for (size_t i = 0; i < blocks[n].succ.size(); i++)
                        {
                            auto entry = std::find(entryList.begin(), entryList.end(), blocks[n].succ[i]);
                            if (entry == entryList.end())
                                continue;

                            Block edge;
                            edge.id = nextId++;
                            edge.body = fmt::format("\t__state = {};\n", entry - entryList.begin());
                            edge.succ.push_back(dispatcher);
                            blocks[n].succ[i] = int(blocks.size());
                            blocks.push_back(std::move(edge));
                            stats.dispatchEdges++;
                        }
                    }

                    stats.irreducibleRegions++;

                    // The new edge blocks that stayed inside the cycle belong to
                    // it, but they are straight lines into the dispatcher, which
                    // is left out of what is searched next.
                }

                MakeReducible(blocks, inner, nextId, stats);
            }
        }

        // Step 2.
        struct Layout
        {
            const std::vector<Block>& blocks;
            const Dominators& d;
            std::vector<bool> isJoin;
            std::vector<bool> isLoop;
            std::vector<std::vector<int>> joinChildren; // highest reverse postorder first
            std::string out;
            size_t depth = 0;
            Stats& stats;

            Layout(const std::vector<Block>& blocks, const Dominators& d, Stats& stats) : blocks(blocks), d(d), stats(stats)
            {
                isJoin.assign(blocks.size(), false);
                isLoop.assign(blocks.size(), false);
                joinChildren.resize(blocks.size());

                std::vector<int> forward(blocks.size(), 0);
                for (int n : d.order)
                {
                    for (int s : blocks[n].succ)
                    {
                        if (s == RETURN)
                            continue;
                        if (d.number[s] > d.number[n])
                            forward[s]++;
                        else
                            isLoop[s] = true;
                    }
                }

                for (auto it = d.order.rbegin(); it != d.order.rend(); ++it)
                {
                    if (forward[*it] >= 2)
                    {
                        isJoin[*it] = true;
                        joinChildren[d.idom[*it]].push_back(*it);
                    }
                }
            }

            void Open(const std::string& line)
            {
                out += line;
                out += '\n';
                stats.maxDepth = std::max(stats.maxDepth, ++depth);
            }

            void Close()
            {
                out += "}\n";
                depth--;
            }

            void Edge(int from, int to)
            {
                if (to == RETURN)
                    out += "return;\n";
                else if (d.number[to] <= d.number[from])
                    out += fmt::format("continue 'l{};\n", blocks[to].id);
                else if (isJoin[to])
                    out += fmt::format("break 'b{};\n", blocks[to].id);
                else
                    Place(to);
            }

            void Terminator(int n)
            {
                const Block& block = blocks[n];
                switch (block.term)
                {
                case Term::Jump:
                    Edge(n, block.succ.empty() ? RETURN : block.succ[0]);
                    break;

                case Term::Cond:
                    if (block.succ[0] == block.succ[1])
                    {
                        Edge(n, block.succ[0]);
                        break;
                    }
                    Open(fmt::format("if {} {{", block.cond));
                    Edge(n, block.succ[0]);
                    Close();
                    Edge(n, block.succ[1]);
                    break;

                case Term::Switch:
                    Open(fmt::format("match {} {{", block.cond));
                    for (size_t i = 0; i < block.succ.size(); i++)
                    {
                        Open(fmt::format("{} => {{", block.values[i]));
                        Edge(n, block.succ[i]);
                        Close();
                    }
                    out += "_ => return,\n";
                    Close();
                    break;
                }
            }

            void Place(int n)
            {
                const auto& joins = joinChildren[n];

                if (isLoop[n])
                    Open(fmt::format("'l{}: loop {{", blocks[n].id));

                for (int join : joins)
                    Open(fmt::format("'b{}: {{", blocks[join].id));

                out += blocks[n].body;
                Terminator(n);

                for (auto join = joins.rbegin(); join != joins.rend(); ++join)
                {
                    Close();
                    Place(*join);
                }

                if (isLoop[n])
                    Close();
            }
        };
    }

    std::string Structure(std::vector<Block> blocks, Stats* stats)
    {
        Stats local;
        Stats& s = stats != nullptr ? *stats : local;

        DropUnreachable(blocks);

        Dominators d = ComputeDominators(blocks);
        if (!IsReducible(blocks, d))
        {
            uint32_t nextId = 0;
            for (const Block& block : blocks)
                nextId = std::max(nextId, block.id + 1);

            std::set<int> all;
            for (size_t n = 0; n < blocks.size(); n++)
                all.insert(int(n));

            MakeReducible(blocks, all, nextId, s);

            d = ComputeDominators(blocks);
            if (!IsReducible(blocks, d))
                throw std::logic_error("the control flow graph is still irreducible");
        }

        Layout layout(blocks, d, s);
        layout.out.reserve(blocks.size() * 128);
        layout.Place(0);
        return std::move(layout.out);
    }
}
