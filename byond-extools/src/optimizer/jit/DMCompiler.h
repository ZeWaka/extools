#pragma once

#include "../../core/core.h"
#include "../../third_party/asmjit/asmjit.h"

#include <stdint.h>
#include <vector>
#include <array>

namespace dmjit
{
	using namespace asmjit;

class ProcNode;
class ProcEndNode;
class BlockNode;
class BlockEndNode;

enum class NodeTypes : uint32_t
{
	kNodeProc = BaseNode::kNodeUser,
	kNodeProcEnd,
	kNodeBlock,
	kNodeBlockEnd,
};

// Reference to a DM variable through operands
struct Variable
{
	Operand Type;
	Operand Value;
};

// Reference to a local DM variable through operands
struct Local
{
	enum class CacheState
	{
		// We have operands representing the latest data in our cache
		Ok,

		// We haven't fetched this value from the JitContext stack yet
		Stale,

		// We've modified this in our cache but not committed it yet.
		Modified,
	};

	CacheState State;
	Variable Variable;
};

class DMCompiler
	: public x86::Compiler
{
public:
	DMCompiler(CodeHolder& holder);

public:
	ProcNode* addProc(uint32_t locals_count, uint32_t args_count);
	void endProc();

	BlockNode* addBlock(Label& label, uint32_t continuation_index = -1);
	void endBlock();

	Variable getLocal(uint32_t index);
	void setLocal(uint32_t index, Variable& variable);

	Variable getArg(uint32_t index);

	Variable getFrameEmbeddedValue(uint32_t offset);

	Variable getSrc();
	Variable getUsr();

	Variable getDot();
	void setDot(Variable& variable);

	template<std::size_t I>
	std::array<Variable, I> popStack()
	{
		if (_currentBlock == nullptr)
			__debugbreak();
		BlockNode& block = *_currentBlock;

		std::array<Variable, I> res;
		int popped_count = 0;

		// The stack cache could be empty if something was pushed to it before jumping to a new block
		for (popped_count; popped_count < I && !block._stack.empty(); popped_count++)
		{
			res[I - popped_count - 1] = block._stack.pop(); // Pop and place in correct order
		}
		_currentBlock->_stack_top_offset -= popped_count;

		if (popped_count == I)
		{
			return res;
		}

		int diff = I - popped_count;

		setInlineComment("popStack (overpopped)");

		auto stack_top = block._stack_top;

		for (popped_count; popped_count < I && _currentBlock->_stack_top_offset - popped_count >= 0; popped_count++)
		{
			auto type = newUInt32();
			auto value = newUInt32();
			mov(type, x86::ptr(stack_top, (block._stack_top_offset - popped_count) * sizeof(Value) - sizeof(Value), sizeof(uint32_t)));
			mov(value, x86::ptr(stack_top, (block._stack_top_offset - popped_count) * sizeof(Value) - offsetof(Value, value), sizeof(uint32_t)));
			res[I - popped_count - 1] = { type, value };
		}
		if (popped_count == I)
		{
			_currentBlock->_stack_top_offset -= diff;
			return res;
		}

		Core::Alert("Failed to pop enough arguments from the stack");
		abort();
	}

	Variable popStack()
	{
		return popStack<1>()[0];
	}


	void pushStack(Variable& variable);
	void clearStack();

	Variable pushStack2();
	Variable pushStack2(Operand type, Operand value);

	// Commits the temporary stack variables - this is called automatically when a block ends
	void commitStack();

	// Commits the local variables to memory - you have to call this before anything that might yield!
	void commitLocals();

	void jump_zero(Label label);

	void jump(Label label);

	x86::Gp getStackFrame();

	x86::Gp getCurrentIterator();
	void setCurrentIterator(Operand iter);

	// Returns the value at the top of the stack
	void doReturn();

private:
	ProcNode* _currentProc;
	BlockNode* _currentBlock;
};


class BlockNode
	: public BaseNode
{
public:
	BlockNode(BaseBuilder* cb, Label& label)
		: BaseNode(cb, static_cast<uint32_t>(NodeTypes::kNodeBlock), kFlagHasNoEffect)
		, _label(label)
		, _stack_top_offset(0)
		, _end(nullptr)
	{
		DMCompiler& dmc = *static_cast<DMCompiler*>(cb);

		_stack_top = dmc.newUIntPtr();
		cb->_newNodeT<BlockEndNode>(&_end);
		//_stack.init(&cb->_allocator);		
	}

	Label _label;
	x86::Gp _stack_top;
	int32_t _stack_top_offset;
	ZoneVector<Variable> _stack;

	BlockEndNode* _end;	
};

class BlockEndNode
	: public BaseNode
{
public:
	BlockEndNode(BaseBuilder* cb)
		: BaseNode(cb, static_cast<uint32_t>(NodeTypes::kNodeBlockEnd), kFlagHasNoEffect)
	{}
};


// Represents an entire compiled proc
class ProcNode
	: public BaseNode
{
public:
	ProcNode(BaseBuilder* cb, uint32_t locals_count, uint32_t args_count)
		: BaseNode(cb, static_cast<uint32_t>(NodeTypes::kNodeProc), kFlagHasNoEffect)
		, _locals_count(locals_count)
		, _args_count(args_count)
		, _end(nullptr)
	{
		DMCompiler& dmc = *static_cast<DMCompiler*>(cb);

		_jit_context = dmc.newUIntPtr();
		_stack_frame = dmc.newUIntPtr();
		_current_iterator = dmc.newUIntPtr();
		_entryPoint = dmc.newLabel();
		_prolog = dmc.newLabel();
		dmc._newNodeT<ProcEndNode>(&_end);

		// Allocate space for all of our locals
		_locals = dmc._allocator.allocT<Local>(locals_count * sizeof(Local));

		// Init the locals
		Local default_local {Local::CacheState::Modified, {Imm(DataType::NULL_D), Imm(0)}};
		for (uint32_t i = 0; i < locals_count; i++)
		{
			_locals[i] = default_local;
		}

		// Do the same for arguments
		_args = dmc._allocator.allocT<Local>(args_count * sizeof(Local));
		for (uint32_t i = 0; i < args_count; i++)
		{
			_args[i] = default_local;
		}

		dmc.xor_(_current_iterator, _current_iterator); // ensure iterator is a nullptr

		//_blocks.reset();
		_continuationPoints.reset();
	}

	x86::Gp _jit_context;
	x86::Gp _stack_frame;
	x86::Gp _current_iterator;

	Label _entryPoint;
	Label _prolog;

	ZoneVector<Label> _continuationPoints;

	Local* _locals;
	uint32_t _locals_count;

	Local* _args;
	uint32_t _args_count;

	// The very very end of our proc. Nothing of this proc exists after this node.
	ProcEndNode* _end;

	// its all our blocks (TODO: maybe not needed)
	//ZoneVector<ProcBlock> _blocks;
};

class ProcEndNode
	: public BaseNode
{
public:
	ProcEndNode(BaseBuilder* cb)
		: BaseNode(cb, static_cast<uint32_t>(NodeTypes::kNodeProcEnd), kFlagHasNoEffect)
	{}
};

}

