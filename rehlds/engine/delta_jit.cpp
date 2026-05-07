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
		unsigned int blockId    = fieldDesc->fieldOffset / 16;
		unsigned int blockStart = blockId * 16;
		unsigned int fieldSize  = DELTAJIT_GetFieldSize(fieldDesc);

		auto jitField = &jitdesc.fields[i];
		jitField->id = i;
		jitField->offset = fieldDesc->fieldOffset;
		jitField->type = fieldDesc->fieldType;
		jitField->length = fieldSize;
		jitField->significantBits = fieldDesc->significant_bits;

		// Walk every 16-byte block this field touches and record a per-byte
		// mask of which bytes within that block belong to this field. The
		// SSE/NEON block fast-path uses these masks to test "did any byte of
		// this field differ" with a single tst against the block's diff mask.
		if ((fieldDesc->fieldType & ~DT_SIGNED) != DT_STRING) {
			bool firstBlock = true;
			deltajit_memblock_field* blockField = nullptr;
			while (blockStart < fieldDesc->fieldOffset + fieldSize) {
				deltajit_memblock* memblock = &jitdesc.blocks[blockId];
				uint32 mask = DELTAJIT_CreateMask(
					fieldDesc->fieldOffset - blockStart,
					fieldDesc->fieldOffset + fieldSize - blockStart);
				blockField = &memblock->fields[memblock->numFields++];
				blockField->field = jitField;
				jitField->numBlocks++;
				blockField->first = firstBlock;
				blockField->mask  = (uint16)mask;

				blockStart += 16;
				blockId++;
				firstBlock = false;
			}
			if (blockField) {
				blockField->last = true;
			}
		}
	}

	// Iteration order: only blocks that contain at least one tracked field.
	for (unsigned int i = 0; i < jitdesc.numblocks; i++) {
		if (jitdesc.blocks[i].numFields > 0) {
			auto* itr = &jitdesc.itrBlocks[jitdesc.numItrBlocks++];
			itr->memblockId      = (int)i;
			itr->memblock        = &jitdesc.blocks[i];
			itr->prefetchBlockId = -1;
		}
	}

	// Schedule a prefetch 4 iteration-blocks ahead. Stops when it would walk
	// off the end. Hides L1 misses on the next-but-three block.
	for (unsigned int i = 0; i < jitdesc.numItrBlocks; i++) {
		unsigned int prefetchBlkId = (i + 1) * 4;
		if (prefetchBlkId >= jitdesc.numblocks)
			break;
		jitdesc.itrBlocks[i].prefetchBlockId = (int)prefetchBlkId;
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
// x86 / x86_64 backend (SSE2 block fast-path)
//
// Mirrors the original jitasm-based codegen: process 16 bytes per pcmpeqb,
// extract a 16-bit "differs" mask via pmovmskb, and resolve each tracked
// field with a single test against its byte-mask within the block. Prefetch
// runs 4 blocks ahead. SSE2 is mandatory on x86_64 and on every x86 CPU
// since ~2003, so we don't gate at runtime.
// =========================================================================
#if REHLDS_JIT_BACKEND_X86

namespace asmx = asmjit::x86;

// Emit the SSE2 block compare for one 16-byte memblock. After this, blockMask
// holds a 16-bit bitmap where bit i = 1 iff byte i within the block differs
// between src and dst. itrIdx is used to select an unrolled prefetch target.
static void x86_emit_block_compare(asmjit::x86::Compiler &cc,
	const asmx::Gp &src, const asmx::Gp &dst,
	const deltajit_memblock_itr_t *itr, const asmx::Gp &blockMask)
{
	int blockOff = itr->memblockId * 16;
	if (itr->prefetchBlockId != -1) {
		int prefetchOff = itr->prefetchBlockId * 16;
		cc.prefetcht0(asmx::byte_ptr(src, prefetchOff));
		cc.prefetcht0(asmx::byte_ptr(dst, prefetchOff));
	}
	asmx::Vec src_xmm = cc.new_vec128();
	asmx::Vec dst_xmm = cc.new_vec128();
	cc.movdqu(src_xmm, asmx::xmmword_ptr(src, blockOff));
	cc.movdqu(dst_xmm, asmx::xmmword_ptr(dst, blockOff));
	cc.pcmpeqb(src_xmm, dst_xmm);          // 0xFF where bytes equal
	cc.pmovmskb(blockMask, src_xmm);        // low 16 bits: equality bitmap
	cc.not_(blockMask);                     // flip: 1 where bytes differ
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

	// Phase 1: SSE2 block iteration builds a 64-bit "field changed" mask in
	// (markedLo, markedHi). Idempotent OR handles fields that span 16-byte
	// boundaries — if ANY of a field's blocks shows a byte diff, its bit is
	// set in the mask. Phase 2 below iterates fields once over the mask.
	asmx::Gp markedLo = cc.new_gp32("markedLo");
	asmx::Gp markedHi = cc.new_gp32("markedHi");
	cc.xor_(markedLo, markedLo);
	cc.xor_(markedHi, markedHi);

	for (unsigned i = 0; i < jd->numItrBlocks; i++) {
		auto *itr = &jd->itrBlocks[i];
		auto *block = itr->memblock;
		asmx::Gp blockMask = cc.new_gp32();
		x86_emit_block_compare(cc, src, dst, itr, blockMask);

		for (unsigned j = 0; j < block->numFields; j++) {
			auto *bf = &block->fields[j];
			deltajit_field *field = bf->field;
			asmjit::Label skip = cc.new_label();
			cc.test(blockMask, (uint32_t)bf->mask);
			cc.jz(skip);
			uint32 bit = 1u << (field->id & 31);
			if (field->id < 32) cc.or_(markedLo, bit);
			else                cc.or_(markedHi, bit);
			cc.bind(skip);
		}
	}

	// Phase 2: score each non-string field once against the union mask.
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) == DT_STRING) continue;

		uint32 bit = 1u << (field->id & 31);
		asmjit::Label not_changed = cc.new_label();
		cc.test(field->id < 32 ? markedLo : markedHi, bit);
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

	// SSE2 block iteration: for each 16-byte memblock, pcmpeqb+pmovmskb to
	// get a 16-bit "differs" map, then OR the matching bit into the marked
	// mask for every field whose bytes overlap the differing region. OR is
	// idempotent so multi-block fields land in the right place even when
	// only some of their bytes differ.
	for (unsigned i = 0; i < jd->numItrBlocks; i++) {
		auto *itr = &jd->itrBlocks[i];
		auto *block = itr->memblock;
		asmx::Gp blockMask = cc.new_gp32();
		x86_emit_block_compare(cc, src, dst, itr, blockMask);

		for (unsigned j = 0; j < block->numFields; j++) {
			auto *bf = &block->fields[j];
			deltajit_field *field = bf->field;
			asmjit::Label skip = cc.new_label();
			cc.test(blockMask, (uint32_t)bf->mask);
			cc.jz(skip);
			uint32 bit = 1u << (field->id & 31);
			if (field->id < 32) cc.or_(markedLo, bit);
			else                cc.or_(markedHi, bit);
			cc.bind(skip);
		}
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
// aarch64 backend (NEON block fast-path)
//
// NEON has direct equivalents for movdqu (ldr q), pcmpeqb (cmeq.16b), and
// prefetcht0 (prfm pldl1keep). It does NOT have a single-instruction
// equivalent for pmovmskb. We synthesize a 16-bit "any byte differs" mask
// via the standard NEON idiom: AND the byte-diff vector with a fixed
// {1,2,4,8,...} bit-position constant, then sum each half via addv to get
// one byte per half, and pack the two bytes into a GP. ~5 NEON ops total.
// =========================================================================
#if REHLDS_JIT_BACKEND_ARM64

namespace asma = asmjit::a64;

// Bit-position constant used to materialize a 16-bit pmovmskb-style mask
// from a NEON byte-diff vector. After AND with this constant, byte i becomes
// 2^(i mod 8) iff the original diff byte was 0xFF; addv across each half
// then sums those into a single byte = the corresponding mask byte.
alignas(16) static const uint8_t kPmovmskbBitConst[16] = {
	1, 2, 4, 8, 16, 32, 64, 128,
	1, 2, 4, 8, 16, 32, 64, 128
};

// Emit the NEON block compare for one 16-byte memblock. After this,
// blockMask is a 16-bit value where bit i = 1 iff byte i within the block
// differs between src and dst. bit_const is a pre-loaded vec128 holding
// kPmovmskbBitConst (load it once at function entry, reuse per block).
static void a64_emit_block_compare(asmjit::a64::Compiler &cc,
	const asma::Gp &src, const asma::Gp &dst,
	const deltajit_memblock_itr_t *itr,
	const asma::Vec &bit_const,
	const asma::Gp &blockMask)
{
	int blockOff = itr->memblockId * 16;
	if (itr->prefetchBlockId != -1) {
		int prefetchOff = itr->prefetchBlockId * 16;
		cc.prfm(asmjit::Imm((uint32_t)asmjit::a64::Predicate::PRFOp::kPLDL1KEEP),
			asma::ptr(src, prefetchOff));
		cc.prfm(asmjit::Imm((uint32_t)asmjit::a64::Predicate::PRFOp::kPLDL1KEEP),
			asma::ptr(dst, prefetchOff));
	}

	asma::Vec src_v = cc.new_vec128();
	asma::Vec dst_v = cc.new_vec128();
	cc.ldr(src_v, asma::ptr(src, blockOff));
	cc.ldr(dst_v, asma::ptr(dst, blockOff));
	cc.cmeq(src_v.b16(), src_v.b16(), dst_v.b16());   // 0xFF where bytes equal
	cc.mvn(src_v.b16(), src_v.b16());                  // 0xFF where bytes differ
	cc.and_(src_v.b16(), src_v.b16(), bit_const.b16());// each byte: 0 or 2^(i mod 8)

	// addv on .8b reduces the low 8 bytes to a single byte at lane 0.
	asma::Vec lo_v = cc.new_vec128();
	cc.addv(lo_v.b(), src_v.b8());

	// ext rotates the high half into the low position; addv again.
	asma::Vec hi_v = cc.new_vec128();
	cc.ext(hi_v.b16(), src_v.b16(), src_v.b16(), 8);
	cc.addv(hi_v.b(), hi_v.b8());

	// Pack two bytes into the 16-bit GP mask.
	cc.umov(blockMask, lo_v.b(0));
	asma::Gp tmp = cc.new_gp32();
	cc.umov(tmp, hi_v.b(0));
	cc.lsl(tmp, tmp, 8);
	cc.orr(blockMask, blockMask, tmp);
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

	// Load the pmovmskb bit-position constant once for the whole function.
	asma::Gp const_addr = cc.new_gpz();
	cc.mov(const_addr, (uint64_t)(uintptr_t)kPmovmskbBitConst);
	asma::Vec bit_const = cc.new_vec128();
	cc.ldr(bit_const, asma::ptr(const_addr));

	// Phase 1: NEON block iteration → markedLo/markedHi (64-bit field mask).
	asma::Gp markedLo = cc.new_gp32("markedLo");
	asma::Gp markedHi = cc.new_gp32("markedHi");
	cc.mov(markedLo, 0);
	cc.mov(markedHi, 0);

	for (unsigned i = 0; i < jd->numItrBlocks; i++) {
		auto *itr = &jd->itrBlocks[i];
		auto *block = itr->memblock;
		asma::Gp blockMask = cc.new_gp32();
		a64_emit_block_compare(cc, src, dst, itr, bit_const, blockMask);

		for (unsigned j = 0; j < block->numFields; j++) {
			auto *bf = &block->fields[j];
			deltajit_field *field = bf->field;
			asmjit::Label skip = cc.new_label();
			asma::Gp test_tmp = cc.new_gp32();
			cc.mov(test_tmp, (uint32_t)bf->mask);
			cc.and_(test_tmp, blockMask, test_tmp);
			cc.cbz(test_tmp, skip);
			uint32 bit = 1u << (field->id & 31);
			if (field->id < 32) cc.orr(markedLo, markedLo, bit);
			else                cc.orr(markedHi, markedHi, bit);
			cc.bind(skip);
		}
	}

	// Phase 2: score each non-string field once against the union mask.
	for (unsigned i = 0; i < jd->numFields; i++) {
		deltajit_field *field = &jd->fields[i];
		if ((field->type & ~DT_SIGNED) == DT_STRING) continue;

		uint32 bit = 1u << (field->id & 31);
		asmjit::Label not_changed = cc.new_label();
		asma::Gp tmp = cc.new_gp32();
		cc.mov(tmp, bit);
		cc.and_(tmp, field->id < 32 ? markedLo : markedHi, tmp);
		cc.cbz(tmp, not_changed);

		asma::Gp idImm = cc.new_gp32();
		cc.mov(idImm, (int)field->id);
		cc.cmp(highestBit, idImm);
		cc.csel(highestBit, idImm, highestBit, asmjit::arm::CondCode::kLT);
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

	// Load pmovmskb bit-position constant once.
	asma::Gp const_addr = cc.new_gpz();
	cc.mov(const_addr, (uint64_t)(uintptr_t)kPmovmskbBitConst);
	asma::Vec bit_const = cc.new_vec128();
	cc.ldr(bit_const, asma::ptr(const_addr));

	// NEON block iteration: build union mask via per-block diff vec → 16-bit
	// GP mask → per-field bit-OR. OR is idempotent so multi-block fields land
	// in the right place.
	for (unsigned i = 0; i < jd->numItrBlocks; i++) {
		auto *itr = &jd->itrBlocks[i];
		auto *block = itr->memblock;
		asma::Gp blockMask = cc.new_gp32();
		a64_emit_block_compare(cc, src, dst, itr, bit_const, blockMask);

		for (unsigned j = 0; j < block->numFields; j++) {
			auto *bf = &block->fields[j];
			deltajit_field *field = bf->field;
			asmjit::Label skip = cc.new_label();
			asma::Gp test_tmp = cc.new_gp32();
			cc.mov(test_tmp, (uint32_t)bf->mask);
			cc.and_(test_tmp, blockMask, test_tmp);
			cc.cbz(test_tmp, skip);
			uint32 bit = 1u << (field->id & 31);
			if (field->id < 32) cc.orr(markedLo, markedLo, bit);
			else                cc.orr(markedHi, markedHi, bit);
			cc.bind(skip);
		}
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
