/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include "gensim/ExecutionEngine.h"

#include <cassert>

using namespace archsim;

DeclareLogContext(LogExecutionEngine, "ExecutionEngine");

class thread_exception {
public:
	thread_exception(uint64_t, uint64_t) {}
};

void execute_thread(BasicExecutionEngine::ExecutionContext *ctx) {
	while(!ctx->should_halt && ctx->last_result == ExecutionResult::Continue) {
		ctx->last_result = ctx->engine->StepThreadBlock(ctx->thread_instance);
		
		assert(ctx->last_result != ExecutionResult::Exception);
	}
}

void BasicExecutionEngine::StartAsyncThread(ThreadInstance* thread)
{
	ExecutionContext *ctx = new ExecutionContext();
	ctx->should_halt = false;
	ctx->last_result = ExecutionResult::Continue;
	ctx->thread_instance = thread;
	ctx->engine = this;
	
	ctx->thread = new std::thread(execute_thread, ctx);
		
	threads_[thread] = ctx;
}

void BasicExecutionEngine::HaltAsyncThread(ThreadInstance* thread)
{
	threads_.at(thread)->should_halt = true;
	threads_.at(thread)->thread->join();
}

ExecutionResult BasicExecutionEngine::JoinAsyncThread(ThreadInstance* thread)
{
	threads_.at(thread)->thread->join();
}

ExecutionResult BasicExecutionEngine::Execute(ThreadInstance* thread)
{
	ExecutionResult result;
	do {
		result = StepThreadBlock(thread);
	} while(result == ExecutionResult::Continue);
}

void BasicExecutionEngine::TakeException(ThreadInstance* thread, uint64_t category, uint64_t data)
{
	assert(threads_.count(thread) == 0 || threads_.at(thread)->thread->get_id() == std::this_thread::get_id());
	
	auto result = thread->TakeException(category, data);
	if(result == archsim::abi::AbortInstruction || result == archsim::abi::AbortSimulation) {
		throw thread_exception(category, data);
	}
}

ExecutionResult BasicExecutionEngine::StepThreadBlock(ThreadInstance* thread)
{
	try {
		return ArchStepBlock(thread);
	} catch(thread_exception &exception) {
		return ExecutionResult::Exception;
	}
}
ExecutionResult BasicExecutionEngine::StepThreadSingle(ThreadInstance* thread)
{
	try {
		return ArchStepSingle(thread);
	} catch(thread_exception &exception) {
		return ExecutionResult::Exception;
	}
}