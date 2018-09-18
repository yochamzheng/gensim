/* This file is Copyright University of Edinburgh 2018. For license details, see LICENSE. */

#include "gensim/gensim_translate.h"
#include "translate/llvm/LLVMTranslationContext.h"
#include "translate/llvm/LLVMAliasAnalysis.h"

#include <llvm/IR/Constants.h>

using namespace gensim;

llvm::Value *BaseLLVMTranslate::EmitRegisterRead(llvm::IRBuilder<> &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int size_in_bytes, int offset)
{
	return ctx.LoadGuestRegister(builder, offset, size_in_bytes);
//	llvm::Value *ptr = ctx.GetRegPtr(offset, llvm::IntegerType::getIntNTy(ctx.LLVMCtx, size_in_bytes*8));
//	auto value = builder.CreateLoad(ptr);
//	return value;
}

bool BaseLLVMTranslate::EmitRegisterWrite(llvm::IRBuilder<> &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int size_in_bytes, int offset, llvm::Value *value)
{
	ctx.StoreGuestRegister(builder, offset, size_in_bytes, value);

//	llvm::Value *ptr = ctx.GetRegPtr(offset, llvm::IntegerType::getIntNTy(ctx.LLVMCtx, size_in_bytes*8));
//	value = builder.CreateBitCast(value, llvm::IntegerType::getIntNTy(ctx.LLVMCtx, size_in_bytes*8));
//	builder.CreateStore(value, ptr);
//	return true;
}

//#define FAST_READS
#define FASTER_READS
#define FASTER_WRITES
//#define FAST_WRITES

llvm::Value* BaseLLVMTranslate::EmitMemoryRead(llvm::IRBuilder<> &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int interface, int size_in_bytes, llvm::Value* address)
{
#ifdef FASTER_READS
	llvm::Value *mem_offset = llvm::ConstantInt::get(ctx.Types.i64, 0x80000000);
	address = builder.CreateAdd(address, mem_offset);
	llvm::IntToPtrInst *ptr = (llvm::IntToPtrInst *)builder.CreateCast(llvm::Instruction::IntToPtr, address, llvm::IntegerType::getIntNPtrTy(ctx.LLVMCtx, size_in_bytes*8, 0));
	if(ptr->getValueID() == llvm::Instruction::InstructionVal + llvm::Instruction::IntToPtr) {
		((llvm::IntToPtrInst*)ptr)->setMetadata("aaai", llvm::MDNode::get(ctx.LLVMCtx, { llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(ctx.Types.i32, archsim::translate::translate_llvm::TAG_MEM_ACCESS)) }));
	}
	auto value = builder.CreateLoad(ptr);

	llvm::Function *trace_fn_ptr = nullptr;
	switch(size_in_bytes) {
		case 1:
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead8;
			break;
		case 2:
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead16;
			break;
		case 4:
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead32;
			break;
		case 8:
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead64;
			break;
		default:
			UNIMPLEMENTED;
	}
#else

#ifdef FAST_READS
	llvm::Value *cache_ptr = builder.CreatePtrToInt(ctx.GetStateBlockPointer("memory_cache_" + std::to_string(interface)), ctx.Types.i64);
	llvm::Value *page_index = builder.CreateLShr(address, 12);

	if(archsim::options::Verbose) {
		EmitIncrementCounter(ctx, ctx.GetThread()->GetMetrics().Reads);
	}

	// TODO: get rid of these magic numbers (number of entries in cache, and cache entry size)
	llvm::Value *cache_entry = builder.CreateURem(page_index, llvm::ConstantInt::get(ctx.Types.i64, 1024));
	cache_entry = builder.CreateMul(cache_entry, llvm::ConstantInt::get(ctx.Types.i64, 16));
	cache_entry = builder.CreateAdd(cache_ptr, cache_entry);

	llvm::Value *tag = builder.CreateIntToPtr(cache_entry, ctx.Types.i64Ptr);
	tag = builder.CreateLoad(tag);

	// does tag match? make sure end of memory access falls on correct page
	llvm::Value *offset_page_base = builder.CreateAdd(address, llvm::ConstantInt::get(ctx.Types.i64, size_in_bytes-1));
	offset_page_base = builder.CreateAnd(offset_page_base, ~archsim::Address::PageMask);

	llvm::Value *tag_match = builder.CreateICmpEQ(tag, offset_page_base);
	llvm::BasicBlock *match_block = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	llvm::BasicBlock *nomatch_block = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	llvm::BasicBlock *continue_block = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	auto br = builder.CreateCondBr(tag_match, match_block, nomatch_block);
	auto mdnode = llvm::MDNode::get(ctx.LLVMCtx, {llvm::MDString::get(ctx.LLVMCtx, "branch_weights"), llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(ctx.Types.i32, 95)), llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(ctx.Types.i32, 5))});
	br->setMetadata(llvm::LLVMContext::MD_prof, mdnode);

	builder.SetInsertPoint(match_block);

	llvm::Value *page_offset = builder.CreateAnd(address, archsim::Address::PageMask);
	llvm::Value *ptr = builder.CreateAdd(cache_entry, llvm::ConstantInt::get(ctx.Types.i64, 8));
	ptr = builder.CreateIntToPtr(ptr, ctx.Types.i64Ptr);
	ptr = builder.CreateLoad(ptr);
	ptr = builder.CreateAdd(ptr, page_offset);
	ptr = builder.CreateIntToPtr(ptr, llvm::IntegerType::getIntNPtrTy(ctx.LLVMCtx, size_in_bytes*8));
	llvm::Value *match_value = builder.CreateLoad(ptr);

	if(archsim::options::Verbose) {
		EmitIncrementCounter(ctx, ctx.GetThread()->GetMetrics().ReadHits);
	}

	builder.CreateBr(continue_block);
	builder.SetInsertPoint(nomatch_block);

#endif
	llvm::Function *fn_ptr = nullptr;
	llvm::Function *trace_fn_ptr = nullptr;
	switch(size_in_bytes) {
		case 1:
			fn_ptr = ctx.Functions.blkRead8;
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead8;
			break;
		case 2:
			fn_ptr = ctx.Functions.blkRead16;
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead16;
			break;
		case 4:
			fn_ptr = ctx.Functions.blkRead32;
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead32;
			break;
		case 8:
			fn_ptr = ctx.Functions.blkRead64;
			trace_fn_ptr = ctx.Functions.cpuTraceMemRead64;
			break;
		default:
			UNIMPLEMENTED;
	}


	llvm::Value *nomatch_value = builder.CreateCall(fn_ptr, {ctx.Values.state_block_ptr, address, llvm::ConstantInt::get(ctx.Types.i32, interface)});

#ifdef FAST_READS
	builder.CreateBr(continue_block);

	builder.SetInsertPoint(continue_block);
	auto value = builder.CreatePHI(llvm::IntegerType::getIntNTy(ctx.LLVMCtx, size_in_bytes*8), 2);
	value->addIncoming(match_value, match_block);
	value->addIncoming(nomatch_value, nomatch_block);
#else
	auto value = nomatch_value;
#endif
#endif
	if(archsim::options::Trace) {
		builder.CreateCall(trace_fn_ptr, {ctx.GetThreadPtr(builder), address, value});
	}

	return value;
}

void BaseLLVMTranslate::EmitMemoryWrite(llvm::IRBuilder<> &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int interface, int size_in_bytes, llvm::Value* address, llvm::Value* value)
{
#ifdef FASTER_READS
	llvm::Value *mem_offset = llvm::ConstantInt::get(ctx.Types.i64, 0x80000000);
	address = builder.CreateAdd(address, mem_offset);
	llvm::Value *ptr = builder.CreateCast(llvm::Instruction::IntToPtr, address, llvm::IntegerType::getIntNPtrTy(ctx.LLVMCtx, size_in_bytes*8, 0));
	if(ptr->getValueID() == llvm::Instruction::InstructionVal + llvm::Instruction::IntToPtr) {
		((llvm::IntToPtrInst*)ptr)->setMetadata("aaai", llvm::MDNode::get(ctx.LLVMCtx, { llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(ctx.Types.i32, archsim::translate::translate_llvm::TAG_MEM_ACCESS)) }));
	}

	builder.CreateStore(value, ptr);

	llvm::Function *trace_fn_ptr = nullptr;
	switch(size_in_bytes) {
		case 1:
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite8;
			break;
		case 2:
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite16;
			break;
		case 4:
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite32;
			break;
		case 8:
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite64;
			break;
		default:
			UNIMPLEMENTED;
	}
#else
#ifdef FAST_WRITES
	llvm::Value *cache_ptr = builder.CreatePtrToInt(ctx.GetStateBlockPointer("memory_cache_" + std::to_string(interface)), ctx.Types.i64);
	llvm::Value *page_index = builder.CreateLShr(address, 12);

	if(archsim::options::Verbose) {
		EmitIncrementCounter(ctx, ctx.GetThread()->GetMetrics().Writes);
	}

	// TODO: get rid of these magic numbers (number of entries in cache, and cache entry size)
	llvm::Value *cache_entry = builder.CreateURem(page_index, llvm::ConstantInt::get(ctx.Types.i64, 1024));
	cache_entry = builder.CreateMul(cache_entry, llvm::ConstantInt::get(ctx.Types.i64, 16));
	cache_entry = builder.CreateAdd(cache_ptr, cache_entry);

	llvm::Value *tag = builder.CreateIntToPtr(cache_entry, ctx.Types.i64Ptr);
	tag = builder.CreateLoad(tag);

	// does tag match?
	llvm::Value *offset_page_base = builder.CreateAdd(address, llvm::ConstantInt::get(ctx.Types.i64, size_in_bytes-1));
	offset_page_base = builder.CreateAnd(offset_page_base, ~archsim::Address::PageMask);

	llvm::Value *tag_match = builder.CreateICmpEQ(tag, offset_page_base);
	llvm::BasicBlock *match_block = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	llvm::BasicBlock *nomatch_block = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	llvm::BasicBlock *continue_block = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	auto br = builder.CreateCondBr(tag_match, match_block, nomatch_block);
	auto mdnode = llvm::MDNode::get(ctx.LLVMCtx, {llvm::MDString::get(ctx.LLVMCtx, "branch_weights"), llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(ctx.Types.i32, 95)), llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(ctx.Types.i32, 5))});
	br->setMetadata(llvm::LLVMContext::MD_prof, mdnode);

	builder.SetInsertPoint(match_block);

	llvm::Value *page_offset = builder.CreateAnd(address, archsim::Address::PageMask);
	llvm::Value *ptr = builder.CreateAdd(cache_entry, llvm::ConstantInt::get(ctx.Types.i64, 8));
	ptr = builder.CreateIntToPtr(ptr, ctx.Types.i64Ptr);
	ptr = builder.CreateLoad(ptr);
	ptr = builder.CreateAdd(ptr, page_offset);
	ptr = builder.CreateIntToPtr(ptr, llvm::IntegerType::getIntNPtrTy(ctx.LLVMCtx, size_in_bytes*8));
	builder.CreateStore(value, ptr);

	if(archsim::options::Verbose) {
		EmitIncrementCounter(ctx, ctx.GetThread()->GetMetrics().WriteHits);
	}

	builder.CreateBr(continue_block);


	builder.SetInsertPoint(nomatch_block);
#endif

	llvm::Function *fn_ptr = nullptr;
	llvm::Function *trace_fn_ptr = nullptr;
	switch(size_in_bytes) {
		case 1:
			fn_ptr = ctx.Functions.cpuWrite8;
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite8;
			break;
		case 2:
			fn_ptr = ctx.Functions.cpuWrite16;
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite16;
			break;
		case 4:
			fn_ptr = ctx.Functions.cpuWrite32;
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite32;
			break;
		case 8:
			fn_ptr = ctx.Functions.cpuWrite64;
			trace_fn_ptr = ctx.Functions.cpuTraceMemWrite64;
			break;
		default:
			UNIMPLEMENTED;
	}

	builder.CreateCall(fn_ptr, {GetThreadPtr(ctx), llvm::ConstantInt::get(ctx.Types.i32, interface), address, value});

#ifdef FAST_WRITES
	builder.CreateBr(continue_block);

	builder.SetInsertPoint(continue_block);
#endif
#endif
	if(archsim::options::Trace) {
		builder.CreateCall(trace_fn_ptr, {ctx.GetThreadPtr(builder), address, value});
	}
}

void BaseLLVMTranslate::EmitTakeException(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, llvm::Value* category, llvm::Value* data)
{
	builder.CreateCall(ctx.Functions.TakeException, {ctx.GetThreadPtr(builder), category, data});
}

llvm::Value* BaseLLVMTranslate::GetRegisterPtr(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int size_in_bytes, int offset)
{
	llvm::Value *ptr = GetRegfilePtr(ctx);
	ptr = builder.CreatePtrToInt(ptr, ctx.Types.i64);
	ptr = builder.CreateAdd(ptr, llvm::ConstantInt::get(ctx.Types.i64, offset));
	ptr = builder.CreateIntToPtr(ptr, llvm::IntegerType::getIntNPtrTy(ctx.LLVMCtx, size_in_bytes*8));
	return ptr;
}

llvm::Value* BaseLLVMTranslate::GetRegfilePtr(archsim::translate::tx_llvm::LLVMTranslationContext& ctx)
{
	return ctx.Values.reg_file_ptr;
}

llvm::Value* BaseLLVMTranslate::GetThreadPtr(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx)
{
	return ctx.GetThreadPtr(builder);
}

void BaseLLVMTranslate::EmitAdcWithFlags(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int bits, llvm::Value* lhs, llvm::Value* rhs, llvm::Value* carry)
{
	// TODO: this leads to not very efficient code. It would be better to
	// figure out a better way of handling this, possibly by modifying LLVM
	// (new intrinsic or custom lowering)

	llvm::IntegerType *base_itype = llvm::IntegerType::getIntNTy(ctx.LLVMCtx, bits);
	llvm::IntegerType *itype = llvm::IntegerType::getIntNTy(ctx.LLVMCtx, bits*2);

	lhs = builder.CreateZExtOrTrunc(lhs, base_itype);
	rhs = builder.CreateZExtOrTrunc(rhs, base_itype);
	carry = builder.CreateZExtOrTrunc(carry, base_itype);

	// cast each value to correct bitwidth
	llvm::Value *extended_lhs = builder.CreateZExtOrTrunc(lhs, itype);
	llvm::Value *extended_rhs = builder.CreateZExtOrTrunc(rhs, itype);
	llvm::Value *extended_carry = builder.CreateZExtOrTrunc(carry, itype);

	// produce result
	llvm::Value *extended_result = builder.CreateAdd(extended_rhs, extended_carry);
	extended_result = builder.CreateAdd(extended_lhs, extended_result);

	llvm::Value *base_result = builder.CreateTrunc(extended_result, base_itype);

	// calculate flags
	llvm::Value *Z = builder.CreateICmpEQ(base_result, llvm::ConstantInt::get(base_itype, 0));
	Z = builder.CreateZExt(Z, ctx.Types.i8);

	llvm::Value *N = builder.CreateICmpSLT(base_result, llvm::ConstantInt::get(base_itype, 0));
	N = builder.CreateZExt(N, ctx.Types.i8);

	llvm::Value *C = builder.CreateICmpUGT(extended_result, builder.CreateZExt(llvm::ConstantInt::get(base_itype, -1ull), itype));
	C = builder.CreateZExt(C, ctx.Types.i8);

	llvm::Value *lhs_sign = builder.CreateICmpSLT(lhs, llvm::ConstantInt::get(base_itype, 0));
	llvm::Value *rhs_sign = builder.CreateICmpSLT(rhs, llvm::ConstantInt::get(base_itype, 0));
	llvm::Value *result_sign = builder.CreateICmpSLT(base_result, llvm::ConstantInt::get(base_itype, 0));

	llvm::Value *V_lhs = builder.CreateICmpEQ(lhs_sign, rhs_sign);
	llvm::Value *V_rhs = builder.CreateICmpNE(lhs_sign, result_sign);

	llvm::Value *V = builder.CreateAnd(V_lhs, V_rhs);
	V = builder.CreateZExt(V, ctx.Types.i8);

	auto C_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("C");
	auto V_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("V");
	auto N_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("N");
	auto Z_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("Z");

	EmitRegisterWrite(builder, ctx, 1, C_desc.GetOffset(), C);
	EmitRegisterWrite(builder, ctx, 1, V_desc.GetOffset(), V);
	EmitRegisterWrite(builder, ctx, 1, N_desc.GetOffset(), N);
	EmitRegisterWrite(builder, ctx, 1, Z_desc.GetOffset(), Z);
}

void BaseLLVMTranslate::EmitSbcWithFlags(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int bits, llvm::Value* lhs, llvm::Value* rhs, llvm::Value* carry)
{
	// TODO: this leads to not very efficient code. It would be better to
	// figure out a better way of handling this, possibly by modifying LLVM
	// (new intrinsic or custom lowering)

	llvm::IntegerType *base_itype = llvm::IntegerType::getIntNTy(ctx.LLVMCtx, bits);
	llvm::IntegerType *itype = llvm::IntegerType::getIntNTy(ctx.LLVMCtx, bits*2);

	lhs = builder.CreateZExtOrTrunc(lhs, base_itype);
	rhs = builder.CreateZExtOrTrunc(rhs, base_itype);
	carry = builder.CreateZExtOrTrunc(carry, base_itype);

	// cast each value to correct bitwidth
	llvm::Value *extended_lhs = builder.CreateZExtOrTrunc(lhs, itype);
	llvm::Value *extended_rhs = builder.CreateZExtOrTrunc(rhs, itype);
	llvm::Value *extended_carry = builder.CreateZExtOrTrunc(carry, itype);

	// produce result
	llvm::Value *result = builder.CreateAdd(extended_rhs, extended_carry);
	result = builder.CreateSub(extended_lhs, result);

	llvm::Value *base_result = builder.CreateTrunc(result, base_itype);

	// calculate flags
	llvm::Value *Z = builder.CreateICmpEQ(base_result, llvm::ConstantInt::get(base_itype, 0));
	Z = builder.CreateZExt(Z, ctx.Types.i8);

	llvm::Value *N = builder.CreateICmpSLT(base_result, llvm::ConstantInt::get(base_itype, 0));
	N = builder.CreateZExt(N, ctx.Types.i8);

	llvm::Value *C = builder.CreateICmpUGT(result, builder.CreateZExt(llvm::ConstantInt::get(base_itype, -1ull), itype));
	C = builder.CreateZExt(C, ctx.Types.i8);

	llvm::Value *lhs_sign = builder.CreateICmpSLT(lhs, llvm::ConstantInt::get(base_itype, 0));
	llvm::Value *rhs_sign = builder.CreateICmpSLT(rhs, llvm::ConstantInt::get(base_itype, 0));
	llvm::Value *result_sign = builder.CreateICmpSLT(base_result, llvm::ConstantInt::get(base_itype, 0));

	llvm::Value *V_lhs = builder.CreateICmpNE(lhs_sign, rhs_sign);
	llvm::Value *V_rhs = builder.CreateICmpEQ(rhs_sign, result_sign);

	llvm::Value *V = builder.CreateAnd(V_lhs, V_rhs);
	V = builder.CreateZExt(V, ctx.Types.i8);

	auto C_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("C");
	auto V_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("V");
	auto N_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("N");
	auto Z_desc = ctx.GetArch().GetRegisterFileDescriptor().GetTaggedEntry("Z");

	EmitRegisterWrite(builder, ctx, 1, C_desc.GetOffset(), C);
	EmitRegisterWrite(builder, ctx, 1, V_desc.GetOffset(), V);
	EmitRegisterWrite(builder, ctx, 1, N_desc.GetOffset(), N);
	EmitRegisterWrite(builder, ctx, 1, Z_desc.GetOffset(), Z);
}

void BaseLLVMTranslate::QueueDynamicBlock(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, std::map<uint16_t, llvm::BasicBlock*>& dynamic_blocks, std::list<uint16_t>& dynamic_block_queue, uint16_t queued_block)
{
	if(dynamic_blocks.count(queued_block)) {
		return;
	}
	dynamic_blocks[queued_block] = llvm::BasicBlock::Create(ctx.LLVMCtx, "", builder.GetInsertBlock()->getParent());
	dynamic_block_queue.push_back(queued_block);
}

void BaseLLVMTranslate::EmitTraceBankedRegisterWrite(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int id, llvm::Value* regnum, int size, llvm::Value* value_ptr)
{
	if(archsim::options::Trace) {
		builder.CreateCall(ctx.Functions.cpuTraceBankedRegisterWrite, {ctx.GetThreadPtr(builder), llvm::ConstantInt::get(ctx.Types.i32, id), builder.CreateZExtOrTrunc(regnum, ctx.Types.i32), llvm::ConstantInt::get(ctx.Types.i32, size), value_ptr});
	}
}

void BaseLLVMTranslate::EmitTraceRegisterWrite(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int id, llvm::Value* value)
{
	if(archsim::options::Trace) {
		builder.CreateCall(ctx.Functions.cpuTraceRegisterWrite, {ctx.GetThreadPtr(builder), llvm::ConstantInt::get(ctx.Types.i32, id), builder.CreateZExtOrTrunc(value, ctx.Types.i64)});
	}
}

void BaseLLVMTranslate::EmitTraceBankedRegisterRead(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int id, llvm::Value* regnum, int size, llvm::Value* value_ptr)
{
	if(archsim::options::Trace) {
		builder.CreateCall(ctx.Functions.cpuTraceBankedRegisterRead, {ctx.GetThreadPtr(builder), llvm::ConstantInt::get(ctx.Types.i32, id), builder.CreateZExtOrTrunc(regnum, ctx.Types.i32), llvm::ConstantInt::get(ctx.Types.i32, size), value_ptr});
	}
}

void BaseLLVMTranslate::EmitTraceRegisterRead(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, int id, llvm::Value* value)
{
	if(archsim::options::Trace) {
		builder.CreateCall(ctx.Functions.cpuTraceRegisterRead, {ctx.GetThreadPtr(builder), llvm::ConstantInt::get(ctx.Types.i32, id), builder.CreateZExtOrTrunc(value, ctx.Types.i64)});
	}
}

void BaseLLVMTranslate::EmitIncrementCounter(Builder &builder, archsim::translate::tx_llvm::LLVMTranslationContext& ctx, archsim::util::Counter64& counter, uint32_t value)
{
	llvm::Value *ptr = llvm::ConstantInt::get(ctx.Types.i64, (uint64_t)counter.get_ptr());
	ptr = builder.CreateIntToPtr(ptr, ctx.Types.i64Ptr);
	llvm::Value *val = builder.CreateLoad(ptr);
	val = builder.CreateAdd(val, llvm::ConstantInt::get(ctx.Types.i64, value));
	builder.CreateStore(val, ptr);
}
