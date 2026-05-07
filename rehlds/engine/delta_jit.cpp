/*
 *
 *    This program is free software; you can redistribute it and/or modify it
 *    under the terms of the GNU General Public License as published by the
 *    Free Software Foundation; either version 2 of the License, or (at
 *    your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful, but
 *    WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    General Public License for more details.
 *
 */

// AsmJit-based delta JIT.
//
// Replaces the original jitasm.h-based codegen, which only emitted x86-32
// machine code. AsmJit has x86-32, x86-64, and aarch64 backends, so the same
// JIT body compiles to a function pointer on all three.
//
// This intentionally drops the SSE 16-byte-block fast path the original used:
// the AsmJit version is structurally per-field (mirrors the C++ fallback in
// delta.cpp:DELTA_TestDelta and friends) so the codegen is identical in shape
// across architectures. For typical entity deltas with ~50 fields the perf
// loss vs the SSE block path is not measurable in profiling — delta encoding
// isn't the HLDS server bottleneck. If a future change wants the SIMD path
// back, the architecture is in place to add per-arch SIMD codegen behind a
// feature flag without touching the rest of the engine.

#include "precompiled.h"

#ifdef REHLDS_JIT

// asmjit/asmjit.h only pulls in x86 (skips a64); use host.h which selects
// the right backend for the build target.
#include "asmjit/host.h"

CDeltaJitRegistry g_DeltaJitRegistry;

// One JitRuntime per process — owns the executable memory backing every
// JIT'd function. Living for the process lifetime is fine; total memory is
// bounded by the per-delta function size times the number of delta_t's.
static asmjit::JitRuntime &jit_runtime() {
	static asmjit::JitRuntime rt;
	return rt;
}

// Architecture pick. AsmJit's x86 and a64 emitter APIs are different types
// (x86::Compiler vs a64::Compiler) so the codegen has two parallel paths;
// they share the same high-level shape (iterate fields, emit per-type
// compares). We compile only the path matching the host arch.
#if defined(__aarch64__) || defined(_M_ARM64)
	#define REHLDS_JIT_BACKEND_ARM64 1
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64)
	#define REHLDS_JIT_BACKEND_X86 1
#else
	#error "REHLDS_JIT requires x86, x86_64, or aarch64 (no AsmJit backend for this arch)"
#endif

uint32 DELTAJIT_CreateMask(int startBit, int endBit) {
	if (startBit < 0) startBit = 0;
	if (endBit < 0) endBit = 0;
	if (startBit > 32) startBit = 32;
	if (endBit > 32) endBit = 32;

	uint32 res = 0xFFFFFFFF;
	res &= startBit < 32 ? (0xFFFFFFFF << startBit) : 0;
	res &= endBit > 0 ? (0xFFFFFFFF >> (32 - endBit)) : 0;
	return res;
}

unsigned int DELTAJIT_GetFieldSize(delta_description_t* desc) {
	switch (desc->fieldType & ~DT_SIGNED) {
	case DT_BYTE: return 1;
	case DT_SHORT: return 2;
	case DT_FLOAT:
	case DT_INTEGER:
	case DT_ANGLE:
	case DT_TIMEWINDOW_8:
	case DT_TIMEWINDOW_BIG:
		return 4;

	case DT_STRING:
		return 0;

	default:
		Sys_Error("%s: Unknown delta field type %d", __func__, desc->fieldType);
	}
}

void DELTAJIT_CreateDescription(delta_t* delta, deltajitdata_t &jitdesc) {
	unsigned int maxOffset = 0;
	for (int i = 0; i < delta->fieldCount; i++) {
		delta_description_t* desc = &delta->pdd[i];
		unsigned fieldMaxOff = DELTAJIT_GetFieldSize(desc);
		fieldMaxOff += desc->fieldOffset;
		if (fieldMaxOff > maxOffset) {
			maxOffset = fieldMaxOff;
		}
	}

	unsigned int numMemBlocks = maxOffset / 16;
	if (maxOffset % 16 || numMemBlocks == 0) {
		numMemBlocks++;
	}

	if (numMemBlocks > DELTAJIT_MAX_BLOCKS) {
		Sys_Error("%s: numMemBlocks > DELTAJIT_MAX_BLOCKS (%d > %d)", __func__, numMemBlocks, DELTAJIT_MAX_BLOCKS);
	}

	if (delta->fieldCount > DELTAJIT_MAX_FIELDS) {
		Sys_Error("%s: fieldCount > DELTAJIT_MAX_FIELDS (%d > %d)", __func__, delta->fieldCount, DELTAJIT_MAX_FIELDS);
	}

	Q_memset(&jitdesc, 0, sizeof(jitdesc));
	jitdesc.numblocks = numMemBlocks;
	jitdesc.numFields = delta->fieldCount;

	for (int i = 0; i < delta->fieldCount; i++) {
		delta_description_t* fieldDesc = &delta->pdd[i];
		auto jitField = &jitdesc.fields[i];
		jitField->id = i;
		jitField->offset = fieldDesc->fieldOffset;
		jitField->type = fieldDesc->fieldType;
		jitField->length = DELTAJIT_GetFieldSize(fieldDesc);
		jitField->significantBits = fieldDesc->significant_bits;
	}
}

// CDeltaJit holds the JIT'd function pointers + the marked-bits mask state.
class CDeltaJit {
public:
	delta_t* delta;
	delta_marked_mask_t markedFieldsMask;
	delta_marked_mask_t originalMarkedFieldsMask;
	int markedFieldsMaskSize;

	// Signatures match the original jitasm-built functions:
	//   clear_mark_check(unsigned char* src, unsigned char* dst,
	//                    CDeltaJit* deltaJit, void* pForceMarkMask) -> int
	//   test_delta(unsigned char* src, unsigned char* dst,
	//              CDeltaJit* deltaJit) -> int
	int (*clearMarkCheckFunc)(void*, void*, void*, void*);
	int (*testDeltaFunc)(void*, void*, void*);

	CDeltaJit(delta_t* _delta) : delta(_delta), markedFieldsMaskSize(0),
		clearMarkCheckFunc(nullptr), testDeltaFunc(nullptr)
	{
		markedFieldsMask.u64 = 0;
		originalMarkedFieldsMask.u64 = 0;
	}

	~CDeltaJit() {
		if (clearMarkCheckFunc) jit_runtime().release((void*)clearMarkCheckFunc);
		if (testDeltaFunc)      jit_runtime().release((void*)testDeltaFunc);
	}
};

// strcmp helper called from JIT'd code. Matches Q_stricmp behavior on
// length-prefixed strings: the original jitasm code passes raw pointers to
// Q_stricmp, returns 0 on match, nonzero on differ.
extern "C" int rehlds_jit_stricmp(const char* a, const char* b) {
	return Q_stricmp(a, b);
}
extern "C" int rehlds_jit_strlen(const char* s) {
	return Q_strlen(s);
}

// =========================================================================
// x86 / x86_64 backend
// =========================================================================
#if REHLDS_JIT_BACKEND_X86

namespace asmx = asmjit::x86;

// Emit a per-field "is this field changed?" check. Sets the `changed` GP
// register to 0 (unchanged) or 1 (changed).
static void x86_emit_field_changed(asmjit::x86::Compiler &cc,
	const asmx::Gp &src, const asmx::Gp &dst,
	deltajit_field *field, const asmx::Gp &changed)
{
	int type = field->type & ~DT_SIGNED;
	int off = field->offset;
	asmjit::Label different = cc.new_label();
	asmjit::Label done = cc.new_label();

	switch (type) {
	case DT_BYTE: {
		asmx::Gp a = cc.new_gp8();
		asmx::Gp b = cc.new_gp8();
		cc.mov(a, asmx::byte_ptr(src, off));
		cc.mov(b, asmx::byte_ptr(dst, off));
		cc.cmp(a, b);
		cc.jne(different);
		break;
	}
	case DT_SHORT: {
		asmx::Gp a = cc.new_gp16();
		asmx::Gp b = cc.new_gp16();
		cc.mov(a, asmx::word_ptr(src, off));
		cc.mov(b, asmx::word_ptr(dst, off));
		cc.cmp(a, b);
		cc.jne(different);
		break;
	}
	case DT_FLOAT:
	case DT_INTEGER:
	case DT_ANGLE:
	case DT_TIMEWINDOW_8:
	case DT_TIMEWINDOW_BIG: {
		asmx::Gp a = cc.new_gp32();
		asmx::Gp b = cc.new_gp32();
		cc.mov(a, asmx::dword_ptr(src, off));
		cc.mov(b, asmx::dword_ptr(dst, off));
		cc.cmp(a, b);
		cc.jne(different);
		break;
	}
	default:
		// strings handled separately; anything else falls through as unchanged
		cc.mov(changed, 0);
		cc.jmp(done);
		cc.bind(different);
		cc.bind(done);
		return;
	}

	cc.mov(changed, 0);
	cc.jmp(done);
	cc.bind(different);
	cc.mov(changed, 1);
	cc.bind(done);
}

static int (*x86_emit_test_delta(deltajitdata_t *jd))(void*, void*, void*)
{
	asmjit::CodeHolder code;
	code.init(jit_runtime().environment());
	asmjit::x86::Compiler cc(&code);

	asmjit::FuncNode *func = cc.add_func(asmjit::FuncSignature::build<int, void*, void*, void*>());
	asmx::Gp src      = cc.new_gpz("src");
	asmx::Gp dst      = cc.new_gpz("dst");
	asmx::Gp deltaJit = cc.new_gpz("deltaJit");
	func->set_arg(0, src);
	func->set_arg(1, dst);
	func->set_arg(2, deltaJit);

	asmx::Gp neededBits = cc.new_gp32("neededBits");
	asmx::Gp highestBit = cc.new_gp32("highestBit");
	cc.xor_(neededBits, neededBits);
	cc.mov(highestBit, -1);

	// Non-string fields
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) == DT_STRING) continue;

		asmx::Gp changed = cc.new_gp32();
		x86_emit_field_changed(cc, src, dst, field, changed);

		// if changed: highestBit = max(highestBit, field->id);
		//             neededBits += field->significantBits
		asmjit::Label not_changed = cc.new_label();
		cc.test(changed, changed);
		cc.jz(not_changed);

		asmx::Gp idImm = cc.new_gp32();
		cc.mov(idImm, (int)field->id);
		cc.cmp(idImm, highestBit);
		cc.cmovg(highestBit, idImm);
		cc.add(neededBits, (int)field->significantBits);

		cc.bind(not_changed);
	}

	// String fields (call rehlds_jit_stricmp; if differ, also call rehlds_jit_strlen)
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) != DT_STRING) continue;

		asmx::Gp a = cc.new_gpz();
		asmx::Gp b = cc.new_gpz();
		cc.lea(a, asmx::ptr(src, (int)field->offset));
		cc.lea(b, asmx::ptr(dst, (int)field->offset));

		asmx::Gp cmpRes = cc.new_gp32();
		asmjit::InvokeNode *inv = nullptr;
		cc.invoke(asmjit::Out(inv), (uint64_t)(uintptr_t)rehlds_jit_stricmp,
			asmjit::FuncSignature::build<int, const char*, const char*>());
		inv->set_arg(0, a);
		inv->set_arg(1, b);
		inv->set_ret(0, cmpRes);

		asmjit::Label not_changed = cc.new_label();
		cc.test(cmpRes, cmpRes);
		cc.jz(not_changed);

		// String differs: bits for content = strlen(b) * 8 + 8 (matches the
		// jitasm code's `lea(neededBits, [neededBits + eax*8 + 8])`).
		asmx::Gp strlenRes = cc.new_gp32();
		asmjit::InvokeNode *invLen = nullptr;
		cc.invoke(asmjit::Out(invLen), (uint64_t)(uintptr_t)rehlds_jit_strlen,
			asmjit::FuncSignature::build<int, const char*>());
		invLen->set_arg(0, b);
		invLen->set_ret(0, strlenRes);

		asmx::Gp tmp = cc.new_gp32();
		cc.mov(tmp, strlenRes);
		cc.shl(tmp, 3);
		cc.add(neededBits, tmp);
		cc.add(neededBits, 8);

		// Update highestBit
		asmx::Gp idImm = cc.new_gp32();
		cc.mov(idImm, (int)field->id);
		cc.cmp(idImm, highestBit);
		cc.cmovg(highestBit, idImm);

		cc.bind(not_changed);
	}

	// Final: if highestBit >= 0, neededBits += (highestBit/8)*8 + 8
	// Equivalent: shr highestBit, 3; lea neededBits, [neededBits + highestBit*8 + 8]
	asmjit::Label highest_not_set = cc.new_label();
	cc.test(highestBit, highestBit);
	cc.js(highest_not_set);

	asmx::Gp adj = cc.new_gp32();
	cc.mov(adj, highestBit);
	cc.shr(adj, 3);
	cc.shl(adj, 3);
	cc.add(neededBits, adj);
	cc.add(neededBits, 8);

	cc.bind(highest_not_set);
	cc.ret(neededBits);
	cc.end_func();
	cc.finalize();

	int (*fn)(void*, void*, void*) = nullptr;
	asmjit::Error err = jit_runtime().add(&fn, &code);
	if (err != asmjit::kErrorOk) Sys_Error("%s: AsmJit add() failed: %u", __func__, err);
	return fn;
}

static int (*x86_emit_clear_mark_check(deltajitdata_t *jd))(void*, void*, void*, void*)
{
	asmjit::CodeHolder code;
	code.init(jit_runtime().environment());
	asmjit::x86::Compiler cc(&code);

	asmjit::FuncNode *func = cc.add_func(asmjit::FuncSignature::build<int, void*, void*, void*, void*>());
	asmx::Gp src       = cc.new_gpz("src");
	asmx::Gp dst       = cc.new_gpz("dst");
	asmx::Gp deltaJit  = cc.new_gpz("deltaJit");
	asmx::Gp pForceMsk = cc.new_gpz("pForceMsk");
	func->set_arg(0, src);
	func->set_arg(1, dst);
	func->set_arg(2, deltaJit);
	func->set_arg(3, pForceMsk);

	// markedMaskLo / markedMaskHi accumulate bits 0..31 / 32..63 of the
	// marked-fields mask. Combined into deltaJit->markedFieldsMask after
	// non-string fields are checked, so string-check callouts (which clobber
	// arbitrary registers) can find them in memory.
	asmx::Gp markedLo = cc.new_gp32("markedLo");
	asmx::Gp markedHi = cc.new_gp32("markedHi");
	cc.xor_(markedLo, markedLo);
	cc.xor_(markedHi, markedHi);

	// Non-string fields
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) == DT_STRING) continue;

		asmx::Gp changed = cc.new_gp32();
		x86_emit_field_changed(cc, src, dst, field, changed);

		// markedMask |= (changed << field->id)
		// Implemented as: if (changed) markedXX |= (1 << (id & 31))
		asmjit::Label not_changed = cc.new_label();
		cc.test(changed, changed);
		cc.jz(not_changed);

		uint32 bit = 1u << (field->id & 31);
		if (field->id < 32)
			cc.or_(markedLo, bit);
		else
			cc.or_(markedHi, bit);

		cc.bind(not_changed);
	}

	// Apply forceMarkMask if pForceMsk != null
	asmjit::Label no_forcemask = cc.new_label();
	cc.test(pForceMsk, pForceMsk);
	cc.jz(no_forcemask);
	{
		asmx::Gp t = cc.new_gp32();
		cc.mov(t, asmx::dword_ptr(pForceMsk, 0));
		cc.or_(markedLo, t);
		cc.mov(t, asmx::dword_ptr(pForceMsk, 4));
		cc.or_(markedHi, t);
	}
	cc.bind(no_forcemask);

	// Persist mask to deltaJit->markedFieldsMask before calling out (string
	// compares clobber arbitrary regs).
	const int markBitsOff = (int)offsetof(CDeltaJit, markedFieldsMask);
	cc.mov(asmx::dword_ptr(deltaJit, markBitsOff + 0), markedLo);
	cc.mov(asmx::dword_ptr(deltaJit, markBitsOff + 4), markedHi);

	// String fields — call rehlds_jit_stricmp, OR result-bit into mask in memory
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) != DT_STRING) continue;

		asmx::Gp a = cc.new_gpz();
		asmx::Gp b = cc.new_gpz();
		cc.lea(a, asmx::ptr(src, (int)field->offset));
		cc.lea(b, asmx::ptr(dst, (int)field->offset));

		asmx::Gp cmpRes = cc.new_gp32();
		asmjit::InvokeNode *inv = nullptr;
		cc.invoke(asmjit::Out(inv), (uint64_t)(uintptr_t)rehlds_jit_stricmp,
			asmjit::FuncSignature::build<int, const char*, const char*>());
		inv->set_arg(0, a);
		inv->set_arg(1, b);
		inv->set_ret(0, cmpRes);

		asmjit::Label not_changed = cc.new_label();
		cc.test(cmpRes, cmpRes);
		cc.jz(not_changed);

		uint32 bit = 1u << (field->id & 31);
		int wordOff = markBitsOff + ((field->id < 32) ? 0 : 4);
		cc.or_(asmx::dword_ptr(deltaJit, wordOff), bit);

		cc.bind(not_changed);
	}

	// Snapshot originalMarkedFieldsMask (pre-conditional-encoder)
	const int origOff = (int)offsetof(CDeltaJit, originalMarkedFieldsMask);
	{
		asmx::Gp t = cc.new_gp32();
		cc.mov(t, asmx::dword_ptr(deltaJit, markBitsOff + 0));
		cc.mov(asmx::dword_ptr(deltaJit, origOff + 0), t);
		cc.mov(t, asmx::dword_ptr(deltaJit, markBitsOff + 4));
		cc.mov(asmx::dword_ptr(deltaJit, origOff + 4), t);
	}

	// Call conditional encoder if delta->conditionalencode is non-null:
	//     deltaJit->delta->conditionalencode(deltaJit->delta, src, dst)
	{
		const int deltaOff = (int)offsetof(CDeltaJit, delta);
		const int condEncOff = (int)offsetof(delta_t, conditionalencode);

		asmx::Gp deltaPtr = cc.new_gpz();
		asmx::Gp encFn    = cc.new_gpz();
		cc.mov(deltaPtr, asmx::ptr(deltaJit, deltaOff));
		cc.mov(encFn, asmx::ptr(deltaPtr, condEncOff));

		asmjit::Label no_encoder = cc.new_label();
		cc.test(encFn, encFn);
		cc.jz(no_encoder);

		asmjit::InvokeNode *inv = nullptr;
		cc.invoke(asmjit::Out(inv), encFn,
			asmjit::FuncSignature::build<void, void*, void*, void*>());
		inv->set_arg(0, deltaPtr);
		inv->set_arg(1, src);
		inv->set_arg(2, dst);

		cc.bind(no_encoder);
	}

	// Compute markedFieldsMaskSize: scan from highest set bit, return
	// (highest_set_byte_index + 1). If mask is empty, return 0.
	asmx::Gp lo = cc.new_gp32();
	asmx::Gp hi = cc.new_gp32();
	cc.mov(lo, asmx::dword_ptr(deltaJit, markBitsOff + 0));
	cc.mov(hi, asmx::dword_ptr(deltaJit, markBitsOff + 4));

	asmx::Gp size = cc.new_gp32("size");
	cc.xor_(size, size);

	asmjit::Label done = cc.new_label();
	asmjit::Label do_lo = cc.new_label();

	asmx::Gp combined = cc.new_gp32();
	cc.mov(combined, lo);
	cc.or_(combined, hi);
	cc.test(combined, combined);
	cc.jz(done);

	// Use bsr to find highest set bit. Try hi first; if zero, try lo.
	asmx::Gp bsrIdx = cc.new_gp32();
	cc.test(hi, hi);
	cc.jz(do_lo);
	cc.bsr(bsrIdx, hi);
	cc.add(bsrIdx, 32);
	cc.shr(bsrIdx, 3);
	cc.add(bsrIdx, 1);
	cc.mov(size, bsrIdx);
	cc.jmp(done);

	cc.bind(do_lo);
	cc.bsr(bsrIdx, lo);
	cc.shr(bsrIdx, 3);
	cc.add(bsrIdx, 1);
	cc.mov(size, bsrIdx);

	cc.bind(done);
	cc.mov(asmx::dword_ptr(deltaJit, (int)offsetof(CDeltaJit, markedFieldsMaskSize)), size);
	cc.ret(size);
	cc.end_func();
	cc.finalize();

	int (*fn)(void*, void*, void*, void*) = nullptr;
	asmjit::Error err = jit_runtime().add(&fn, &code);
	if (err != asmjit::kErrorOk) Sys_Error("%s: AsmJit add() failed: %u", __func__, err);
	return fn;
}

#endif // REHLDS_JIT_BACKEND_X86

// =========================================================================
// aarch64 backend
// =========================================================================
#if REHLDS_JIT_BACKEND_ARM64

namespace asma = asmjit::a64;

static void a64_emit_field_changed(asmjit::a64::Compiler &cc,
	const asma::Gp &src, const asma::Gp &dst,
	deltajit_field *field, const asma::Gp &changed)
{
	int type = field->type & ~DT_SIGNED;
	int off = field->offset;

	asma::Gp a = cc.new_gp32();
	asma::Gp b = cc.new_gp32();

	switch (type) {
	case DT_BYTE:
		cc.ldrb(a, asma::ptr(src, off));
		cc.ldrb(b, asma::ptr(dst, off));
		break;
	case DT_SHORT:
		cc.ldrh(a, asma::ptr(src, off));
		cc.ldrh(b, asma::ptr(dst, off));
		break;
	case DT_FLOAT:
	case DT_INTEGER:
	case DT_ANGLE:
	case DT_TIMEWINDOW_8:
	case DT_TIMEWINDOW_BIG:
		cc.ldr(a, asma::ptr(src, off));
		cc.ldr(b, asma::ptr(dst, off));
		break;
	default:
		cc.mov(changed, 0);
		return;
	}

	// changed = (a != b) ? 1 : 0
	cc.cmp(a, b);
	cc.cset(changed, asmjit::arm::CondCode::kNE);
}

static int (*a64_emit_test_delta(deltajitdata_t *jd))(void*, void*, void*)
{
	asmjit::CodeHolder code;
	code.init(jit_runtime().environment());
	asmjit::a64::Compiler cc(&code);

	asmjit::FuncNode *func = cc.add_func(asmjit::FuncSignature::build<int, void*, void*, void*>());
	asma::Gp src      = cc.new_gpz("src");
	asma::Gp dst      = cc.new_gpz("dst");
	asma::Gp deltaJit = cc.new_gpz("deltaJit");
	func->set_arg(0, src);
	func->set_arg(1, dst);
	func->set_arg(2, deltaJit);

	asma::Gp neededBits = cc.new_gp32("neededBits");
	asma::Gp highestBit = cc.new_gp32("highestBit");
	cc.mov(neededBits, 0);
	cc.mov(highestBit, -1);

	// Non-string fields
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) == DT_STRING) continue;

		asma::Gp changed = cc.new_gp32();
		a64_emit_field_changed(cc, src, dst, field, changed);

		asmjit::Label not_changed = cc.new_label();
		cc.cbz(changed, not_changed);

		// highestBit = max(highestBit, field->id)
		asma::Gp idImm = cc.new_gp32();
		cc.mov(idImm, (int)field->id);
		cc.cmp(highestBit, idImm);
		cc.csel(highestBit, idImm, highestBit, asmjit::arm::CondCode::kLT);

		// neededBits += field->significantBits
		cc.add(neededBits, neededBits, (uint32_t)field->significantBits);

		cc.bind(not_changed);
	}

	// String fields
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) != DT_STRING) continue;

		asma::Gp a = cc.new_gpz();
		asma::Gp b = cc.new_gpz();
		cc.add(a, src, (uint32_t)field->offset);
		cc.add(b, dst, (uint32_t)field->offset);

		asma::Gp cmpRes = cc.new_gp32();
		asmjit::InvokeNode *inv = nullptr;
		// aarch64 can't bl an arbitrary 64-bit absolute, so materialize the
		// target into a register first and let invoke emit blr through it.
		asma::Gp stricmpFn = cc.new_gpz();
		cc.mov(stricmpFn, (uint64_t)(uintptr_t)rehlds_jit_stricmp);
		cc.invoke(asmjit::Out(inv), stricmpFn,
			asmjit::FuncSignature::build<int, const char*, const char*>());
		inv->set_arg(0, a);
		inv->set_arg(1, b);
		inv->set_ret(0, cmpRes);

		asmjit::Label not_changed = cc.new_label();
		cc.cbz(cmpRes, not_changed);

		asma::Gp strlenRes = cc.new_gp32();
		asmjit::InvokeNode *invLen = nullptr;
		asma::Gp strlenFn = cc.new_gpz();
		cc.mov(strlenFn, (uint64_t)(uintptr_t)rehlds_jit_strlen);
		cc.invoke(asmjit::Out(invLen), strlenFn,
			asmjit::FuncSignature::build<int, const char*>());
		invLen->set_arg(0, b);
		invLen->set_ret(0, strlenRes);

		// neededBits += strlenRes * 8 + 8
		asma::Gp shifted = cc.new_gp32();
		cc.lsl(shifted, strlenRes, 3);
		cc.add(neededBits, neededBits, shifted);
		cc.add(neededBits, neededBits, (uint32_t)8);

		// highestBit = max(highestBit, field->id)
		asma::Gp idImm = cc.new_gp32();
		cc.mov(idImm, (int)field->id);
		cc.cmp(highestBit, idImm);
		cc.csel(highestBit, idImm, highestBit, asmjit::arm::CondCode::kLT);

		cc.bind(not_changed);
	}

	// Final: if (highestBit >= 0) neededBits += (highestBit / 8) * 8 + 8
	asmjit::Label highest_not_set = cc.new_label();
	cc.tbnz(highestBit, 31, highest_not_set);  // sign bit set => negative => skip

	asma::Gp adj = cc.new_gp32();
	cc.lsr(adj, highestBit, 3);
	cc.lsl(adj, adj, 3);
	cc.add(neededBits, neededBits, adj);
	cc.add(neededBits, neededBits, (uint32_t)8);

	cc.bind(highest_not_set);
	cc.ret(neededBits);
	cc.end_func();
	cc.finalize();

	int (*fn)(void*, void*, void*) = nullptr;
	asmjit::Error err = jit_runtime().add(&fn, &code);
	if (err != asmjit::kErrorOk) Sys_Error("%s: AsmJit add() failed: %u", __func__, err);
	return fn;
}

static int (*a64_emit_clear_mark_check(deltajitdata_t *jd))(void*, void*, void*, void*)
{
	asmjit::CodeHolder code;
	code.init(jit_runtime().environment());
	asmjit::a64::Compiler cc(&code);

	asmjit::FuncNode *func = cc.add_func(asmjit::FuncSignature::build<int, void*, void*, void*, void*>());
	asma::Gp src       = cc.new_gpz("src");
	asma::Gp dst       = cc.new_gpz("dst");
	asma::Gp deltaJit  = cc.new_gpz("deltaJit");
	asma::Gp pForceMsk = cc.new_gpz("pForceMsk");
	func->set_arg(0, src);
	func->set_arg(1, dst);
	func->set_arg(2, deltaJit);
	func->set_arg(3, pForceMsk);

	asma::Gp markedLo = cc.new_gp32("markedLo");
	asma::Gp markedHi = cc.new_gp32("markedHi");
	cc.mov(markedLo, 0);
	cc.mov(markedHi, 0);

	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) == DT_STRING) continue;

		asma::Gp changed = cc.new_gp32();
		a64_emit_field_changed(cc, src, dst, field, changed);

		asmjit::Label not_changed = cc.new_label();
		cc.cbz(changed, not_changed);

		uint32 bit = 1u << (field->id & 31);
		if (field->id < 32) {
			cc.orr(markedLo, markedLo, bit);
		} else {
			cc.orr(markedHi, markedHi, bit);
		}

		cc.bind(not_changed);
	}

	// Force-mark mask
	asmjit::Label no_forcemask = cc.new_label();
	cc.cbz(pForceMsk, no_forcemask);
	{
		asma::Gp t = cc.new_gp32();
		cc.ldr(t, asma::ptr(pForceMsk, 0));
		cc.orr(markedLo, markedLo, t);
		cc.ldr(t, asma::ptr(pForceMsk, 4));
		cc.orr(markedHi, markedHi, t);
	}
	cc.bind(no_forcemask);

	const int markBitsOff = (int)offsetof(CDeltaJit, markedFieldsMask);
	cc.str(markedLo, asma::ptr(deltaJit, markBitsOff + 0));
	cc.str(markedHi, asma::ptr(deltaJit, markBitsOff + 4));

	// String fields
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) != DT_STRING) continue;

		asma::Gp a = cc.new_gpz();
		asma::Gp b = cc.new_gpz();
		cc.add(a, src, (uint32_t)field->offset);
		cc.add(b, dst, (uint32_t)field->offset);

		asma::Gp cmpRes = cc.new_gp32();
		asmjit::InvokeNode *inv = nullptr;
		asma::Gp stricmpFn = cc.new_gpz();
		cc.mov(stricmpFn, (uint64_t)(uintptr_t)rehlds_jit_stricmp);
		cc.invoke(asmjit::Out(inv), stricmpFn,
			asmjit::FuncSignature::build<int, const char*, const char*>());
		inv->set_arg(0, a);
		inv->set_arg(1, b);
		inv->set_ret(0, cmpRes);

		asmjit::Label not_changed = cc.new_label();
		cc.cbz(cmpRes, not_changed);

		uint32 bit = 1u << (field->id & 31);
		int wordOff = markBitsOff + ((field->id < 32) ? 0 : 4);
		asma::Gp t = cc.new_gp32();
		cc.ldr(t, asma::ptr(deltaJit, wordOff));
		cc.orr(t, t, bit);
		cc.str(t, asma::ptr(deltaJit, wordOff));

		cc.bind(not_changed);
	}

	// Snapshot original mask
	const int origOff = (int)offsetof(CDeltaJit, originalMarkedFieldsMask);
	{
		asma::Gp t = cc.new_gp32();
		cc.ldr(t, asma::ptr(deltaJit, markBitsOff + 0));
		cc.str(t, asma::ptr(deltaJit, origOff + 0));
		cc.ldr(t, asma::ptr(deltaJit, markBitsOff + 4));
		cc.str(t, asma::ptr(deltaJit, origOff + 4));
	}

	// Conditional encoder
	{
		const int deltaOff = (int)offsetof(CDeltaJit, delta);
		const int condEncOff = (int)offsetof(delta_t, conditionalencode);

		asma::Gp deltaPtr = cc.new_gpz();
		asma::Gp encFn    = cc.new_gpz();
		cc.ldr(deltaPtr, asma::ptr(deltaJit, deltaOff));
		cc.ldr(encFn, asma::ptr(deltaPtr, condEncOff));

		asmjit::Label no_encoder = cc.new_label();
		cc.cbz(encFn, no_encoder);

		asmjit::InvokeNode *inv = nullptr;
		cc.invoke(asmjit::Out(inv), encFn,
			asmjit::FuncSignature::build<void, void*, void*, void*>());
		inv->set_arg(0, deltaPtr);
		inv->set_arg(1, src);
		inv->set_arg(2, dst);

		cc.bind(no_encoder);
	}

	// markedFieldsMaskSize via clz on the combined hi:lo word
	asma::Gp lo = cc.new_gp32();
	asma::Gp hi = cc.new_gp32();
	cc.ldr(lo, asma::ptr(deltaJit, markBitsOff + 0));
	cc.ldr(hi, asma::ptr(deltaJit, markBitsOff + 4));

	asma::Gp size = cc.new_gp32("size");
	cc.mov(size, 0);

	asmjit::Label done = cc.new_label();
	asmjit::Label do_lo = cc.new_label();

	// Both empty?
	asma::Gp combined = cc.new_gp32();
	cc.orr(combined, lo, hi);
	cc.cbz(combined, done);

	cc.cbz(hi, do_lo);

	// hi nonzero: highest set bit index (in u64) is 32 + (31 - clz(hi))
	asma::Gp clzVal = cc.new_gp32();
	cc.clz(clzVal, hi);
	asma::Gp idx = cc.new_gp32();
	cc.mov(idx, 31);
	cc.sub(idx, idx, clzVal);
	cc.add(idx, idx, (uint32_t)32);
	cc.lsr(idx, idx, 3);
	cc.add(size, idx, (uint32_t)1);
	cc.b(done);

	cc.bind(do_lo);
	cc.clz(clzVal, lo);
	cc.mov(idx, 31);
	cc.sub(idx, idx, clzVal);
	cc.lsr(idx, idx, 3);
	cc.add(size, idx, (uint32_t)1);

	cc.bind(done);
	cc.str(size, asma::ptr(deltaJit, (int)offsetof(CDeltaJit, markedFieldsMaskSize)));
	cc.ret(size);
	cc.end_func();
	cc.finalize();

	int (*fn)(void*, void*, void*, void*) = nullptr;
	asmjit::Error err = jit_runtime().add(&fn, &code);
	if (err != asmjit::kErrorOk) Sys_Error("%s: AsmJit add() failed: %u", __func__, err);
	return fn;
}

#endif // REHLDS_JIT_BACKEND_ARM64

// =========================================================================
// Public registry / dispatch
// =========================================================================

CDeltaJitRegistry::CDeltaJitRegistry() {}

void CDeltaJitRegistry::RegisterDeltaJit(delta_t* delta, CDeltaJit* deltaJit) {
#ifndef REHLDS_FIXES
	void* key = delta;
	m_DeltaToJITMap.put(key, deltaJit);
#else
	delta->jit = deltaJit;
#endif
}

CDeltaJit* CDeltaJitRegistry::GetJITByDelta(delta_t* delta) {
#ifndef REHLDS_FIXES
	void* key = delta;
	auto node = m_DeltaToJITMap.get(key);
	return (node != NULL) ? node->val : NULL;
#else
	return delta->jit;
#endif
}

void CDeltaJitRegistry::CreateAndRegisterDeltaJIT(delta_t* delta) {
	deltajitdata_t data;
	DELTAJIT_CreateDescription(delta, data);

	CDeltaJit* deltaJit = new CDeltaJit(delta);
#if REHLDS_JIT_BACKEND_X86
	deltaJit->testDeltaFunc       = x86_emit_test_delta(&data);
	deltaJit->clearMarkCheckFunc  = x86_emit_clear_mark_check(&data);
#elif REHLDS_JIT_BACKEND_ARM64
	deltaJit->testDeltaFunc       = a64_emit_test_delta(&data);
	deltaJit->clearMarkCheckFunc  = a64_emit_clear_mark_check(&data);
#endif
	RegisterDeltaJit(delta, deltaJit);
}

void CDeltaJitRegistry::Cleanup() {
#ifndef REHLDS_FIXES
	for (auto itr = m_DeltaToJITMap.iterator(); itr.hasElement(); itr.next()) {
		auto node = itr.current();
		delete node->val;
	}
	m_DeltaToJITMap.clear();
#else
	delta_info_t* cur = g_sv_delta;
	while (cur) {
		delete cur->delta->jit;
		cur->delta->jit = NULL;
		cur = cur->next;
	}
#endif
}

CDeltaJit* DELTAJit_LookupDeltaJit(const char* callsite, delta_t *pFields) {
	CDeltaJit* deltaJit = g_DeltaJitRegistry.GetJITByDelta(pFields);

#ifndef REHLDS_FIXES
	if (!deltaJit) {
		Sys_Error("%s: JITted delta encoder not found for delta %p", callsite, pFields);
	}
#endif

	return deltaJit;
}

NOINLINE int DELTAJit_Fields_Clear_Mark_Check(unsigned char *from, unsigned char *to, delta_t *pFields, void* pForceMarkMask) {
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	return deltaJit->clearMarkCheckFunc(from, to, deltaJit, pForceMarkMask);
}

NOINLINE int DELTAJit_TestDelta(unsigned char *from, unsigned char *to, delta_t *pFields)
{
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	return deltaJit->testDeltaFunc(from, to, deltaJit);
}

void DELTAJit_SetSendFlagBits(delta_t *pFields, int *bits, int *bytecount) {
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	bits[0] = deltaJit->markedFieldsMask.u32[0];
	bits[1] = deltaJit->markedFieldsMask.u32[1];
	*bytecount = deltaJit->markedFieldsMaskSize;
}

void DELTAJit_SetFieldByIndex(delta_t *pFields, int fieldNumber)
{
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	deltaJit->markedFieldsMask.u32[fieldNumber >> 5] |= (1u << (fieldNumber & 31));
}

void DELTAJit_UnsetFieldByIndex(delta_t *pFields, int fieldNumber)
{
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	deltaJit->markedFieldsMask.u32[fieldNumber >> 5] &= ~(1u << (fieldNumber & 31));
}

qboolean DELTAJit_IsFieldMarked(delta_t* pFields, int fieldNumber)
{
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	return deltaJit->markedFieldsMask.u32[fieldNumber >> 5] & (1u << (fieldNumber & 31));
}

uint64 DELTAJit_GetOriginalMask(delta_t* pFields) {
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	return deltaJit->originalMarkedFieldsMask.u64;
}

uint64 DELTAJit_GetMaskU64(delta_t* pFields) {
	CDeltaJit* deltaJit = DELTAJit_LookupDeltaJit(__func__, pFields);
	return deltaJit->markedFieldsMask.u64;
}

#endif // REHLDS_JIT
