#pragma once

#include "pch.h"

struct Recompiler;

// The functions that must keep every register in the context although the
// *_as_local options make registers locals: those a value reaches, or leaves,
// in a register the calling convention does not carry. See register_flow.cpp.
std::unordered_set<size_t> FindFunctionsKeepingRegistersInContext(const Recompiler& recompiler);

// --minimal-lr: the functions whose callers must set the return address, because
// it can reach one of `readers`, an import or an indirect call: the readers
// themselves, and every function that tail calls one of those or such a function.
// A non-tail call sets its own return address, so nothing else carries one on.
std::unordered_set<size_t> FindFunctionsNeedingReturnAddress(const Recompiler& recompiler, const std::unordered_set<size_t>& readers);
