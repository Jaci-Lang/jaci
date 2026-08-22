// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "Luau/GlobalTypes.h"

LUAU_FASTFLAG(LuauIntegerType2)
LUAU_FASTFLAG(DebugLuauUserDefinedClasses)

namespace Luau
{

GlobalTypes::GlobalTypes(NotNull<BuiltinTypes> builtinTypes, SolverMode mode)
    : builtinTypes(builtinTypes)
    , mode(mode)
{
    globalScope = std::make_shared<Scope>(globalTypes.addTypePack(TypePackVar{FreeTypePack{TypeLevel{}}}));
    globalTypeFunctionScope = std::make_shared<Scope>(globalTypes.addTypePack(TypePackVar{FreeTypePack{TypeLevel{}}}));

    globalScope->addBuiltinTypeBinding("any", TypeFun{{}, builtinTypes->anyType});
    globalScope->addBuiltinTypeBinding("nil", TypeFun{{}, builtinTypes->nilType});
    globalScope->addBuiltinTypeBinding("unit", TypeFun{{}, builtinTypes->nilType});
    globalScope->addBuiltinTypeBinding("void", TypeFun{{}, builtinTypes->nilType});

    globalScope->addBuiltinTypeBinding("number", TypeFun{{}, builtinTypes->numberType});
    globalScope->addBuiltinTypeBinding("float", TypeFun{{}, builtinTypes->numberType});
    globalScope->addBuiltinTypeBinding("double", TypeFun{{}, builtinTypes->numberType});
    globalScope->addBuiltinTypeBinding("float32", TypeFun{{}, builtinTypes->numberType});
    globalScope->addBuiltinTypeBinding("float64", TypeFun{{}, builtinTypes->numberType});
    globalScope->addBuiltinTypeBinding("f32", TypeFun{{}, builtinTypes->numberType});
    globalScope->addBuiltinTypeBinding("f64", TypeFun{{}, builtinTypes->numberType});

    if (FFlag::LuauIntegerType2)
    {
        globalScope->addBuiltinTypeBinding("integer", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("int", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("int8", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("int16", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("int32", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("int64", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("i8", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("i16", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("i32", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("i64", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("uint", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("uint8", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("uint16", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("uint32", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("uint64", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("u8", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("u16", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("u32", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("u64", TypeFun{{}, builtinTypes->integerType});
        globalScope->addBuiltinTypeBinding("byte", TypeFun{{}, builtinTypes->integerType});
    }
    else
    {
        globalScope->addBuiltinTypeBinding("int", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("int8", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("int16", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("int32", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("int64", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("i8", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("i16", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("i32", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("i64", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("uint", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("uint8", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("uint16", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("uint32", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("uint64", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("u8", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("u16", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("u32", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("u64", TypeFun{{}, builtinTypes->numberType});
        globalScope->addBuiltinTypeBinding("byte", TypeFun{{}, builtinTypes->numberType});
    }

    globalScope->addBuiltinTypeBinding("string", TypeFun{{}, builtinTypes->stringType});
    globalScope->addBuiltinTypeBinding("boolean", TypeFun{{}, builtinTypes->booleanType});
    globalScope->addBuiltinTypeBinding("thread", TypeFun{{}, builtinTypes->threadType});
    globalScope->addBuiltinTypeBinding("buffer", TypeFun{{}, builtinTypes->bufferType});
    globalScope->addBuiltinTypeBinding("unknown", TypeFun{{}, builtinTypes->unknownType});
    globalScope->addBuiltinTypeBinding("never", TypeFun{{}, builtinTypes->neverType});
    if (FFlag::DebugLuauUserDefinedClasses)
    {
        globalScope->addBuiltinTypeBinding("object", TypeFun{{}, builtinTypes->objectType});
        globalScope->addBuiltinTypeBinding("class", TypeFun{{}, builtinTypes->classType});
    }

    unfreeze(*builtinTypes->arena);
    TypeId stringMetatableTy = makeStringMetatable(builtinTypes, mode);
    asMutable(builtinTypes->stringType)->ty.emplace<PrimitiveType>(PrimitiveType::String, stringMetatableTy);
    persist(stringMetatableTy);
    freeze(*builtinTypes->arena);
}

} // namespace Luau
