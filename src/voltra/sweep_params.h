#pragma once
/*
 * Parameter sweep list for the diagnostic build (env:remote_diag, VOLTRA_DIAG=1).
 * Generated from the vendor dictionary by tools/gen_sweep_params.py.
 */
#include <stddef.h>
#include <stdint.h>

#ifndef VOLTRA_DIAG
#define VOLTRA_DIAG 0
#endif

#if VOLTRA_DIAG
namespace voltra {
extern const uint16_t SWEEP_PARAMS[];
extern const size_t SWEEP_PARAM_COUNT;
}  // namespace voltra
#endif
