#pragma once

#include "pch.h"

struct Recompiler;

// The functions that must keep every register in the context although the
// *_as_local options make registers locals: those a value reaches, or leaves,
// in a register the calling convention does not carry. See register_flow.cpp.
std::unordered_set<size_t> FindFunctionsKeepingRegistersInContext(const Recompiler& recompiler);
