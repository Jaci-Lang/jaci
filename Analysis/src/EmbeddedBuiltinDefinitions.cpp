// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/BuiltinDefinitions.h"

LUAU_FASTFLAG(LuauIntegerLibrary)
LUAU_FASTFLAG(LuauIntegerType2)
LUAU_FASTFLAG(LuauAllowGlobalDeclarationToBeCalledClass)
LUAU_FASTFLAG(DebugLuauUserDefinedClasses)

namespace Luau
{

static constexpr const char* kBuiltinDefinitionBaseSrc = R"BUILTIN_SRC(

@checked declare function require(target: any): any

@checked declare function getfenv(target: any): { [string]: any }

declare _G: any
declare _VERSION: string
declare arg: {string}?

declare function gcinfo(): number
declare function collectgarbage(opt: string?, arg: any?): any

declare function print<T...>(...: T...)

declare function type<T>(value: T): string
declare function typeof<T>(value: T): string

-- `assert` has a magic function attached that will give more detailed type information
declare function assert<T>(value: T, errorMessage: string?): T
declare function error<T>(message: T, level: number?): never

declare function tostring<T>(value: T): string
declare function tonumber<T>(value: T, radix: number?): number?

declare function rawequal<T1, T2>(a: T1, b: T2): boolean
declare function rawget<K, V>(tab: {[K]: V}, k: K): V?
declare function rawset<K, V>(tab: {[K]: V}, k: K, v: V): {[K]: V}
declare function rawlen<K, V>(obj: {[K]: V} | string): number

declare function setfenv<T..., R...>(target: number | (T...) -> R..., env: {[string]: any}): ((T...) -> R...)?

declare function ipairs<V>(tab: {V}): (({V}, number) -> (number?, V), {V}, number)

declare function pcall<A..., R...>(f: (A...) -> R..., ...: A...): (boolean, R...)

-- FIXME: The actual type of `xpcall` is:
-- <E, A..., R1..., R2...>(f: (A...) -> R1..., err: (E) -> R2..., A...) -> (true, R1...) | (false, R2...)
-- Since we can't represent the return value, we use (boolean, R1...).
declare function xpcall<E, A..., R1..., R2...>(f: (A...) -> R1..., err: (E) -> R2..., ...: A...): (boolean, R1...)

-- `select` has a magic function attached to provide more detailed type information
declare function select<A...>(i: string | number, ...: A...): ...any

-- FIXME: This type is not entirely correct - `loadstring` returns a function or
-- (nil, string).
declare function loadstring<A...>(src: string, chunkname: string?): (((A...) -> any)?, string?)
declare function loadfile<A...>(filename: string?, chunkname: string?): (((A...) -> any)?, string?)
declare function dofile(filename: string?): ...any

@checked declare function newproxy(mt: boolean?): any

-- Cannot use `typeof` here because it will produce a polytype when we expect a monotype.
declare function unpack<V>(tab: {V}, i: number?, j: number?): ...V

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionBit32Src = R"BUILTIN_SRC(

declare bit32: {
    band: @checked (...number) -> number,
    bor: @checked (...number) -> number,
    bxor: @checked (...number) -> number,
    btest: @checked (number, ...number) -> boolean,
    rrotate: @checked (x: number, disp: number) -> number,
    lrotate: @checked (x: number, disp: number) -> number,
    lshift: @checked (x: number, disp: number) -> number,
    arshift: @checked (x: number, disp: number) -> number,
    rshift: @checked (x: number, disp: number) -> number,
    bnot: @checked (x: number) -> number,
    extract: @checked (n: number, field: number, width: number?) -> number,
    replace: @checked (n: number, v: number, field: number, width: number?) -> number,
    countlz: @checked (n: number) -> number,
    countrz: @checked (n: number) -> number,
    byteswap: @checked (n: number) -> number,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionMathSrc = R"BUILTIN_SRC(

declare math: {
    frexp: @checked (n: number) -> (number, number),
    ldexp: @checked (s: number, e: number) -> number,
    fmod: @checked (x: number, y: number) -> number,
    modf: @checked (n: number) -> (number, number),
    pow: @checked (x: number, y: number) -> number,
    exp: @checked (n: number) -> number,

    ceil: @checked (n: number) -> number,
    floor: @checked (n: number) -> number,
    abs: @checked (n: number) -> number,
    sqrt: @checked (n: number) -> number,

    log: @checked (n: number, base: number?) -> number,
    log10: @checked (n: number) -> number,

    rad: @checked (n: number) -> number,
    deg: @checked (n: number) -> number,

    sin: @checked (n: number) -> number,
    cos: @checked (n: number) -> number,
    tan: @checked (n: number) -> number,
    sinh: @checked (n: number) -> number,
    cosh: @checked (n: number) -> number,
    tanh: @checked (n: number) -> number,
    atan: @checked (n: number) -> number,
    acos: @checked (n: number) -> number,
    asin: @checked (n: number) -> number,
    atan2: @checked (y: number, x: number) -> number,

    min: @checked (number, ...number) -> number,
    max: @checked (number, ...number) -> number,

    pi: number,
    huge: number,
    nan: number,
    e: number,
    phi: number,
    sqrt2: number,
    tau: number,

    randomseed: @checked (seed: number) -> (),
    random: @checked (number?, number?) -> number,

    sign: @checked (n: number) -> number,
    clamp: @checked (n: number, min: number, max: number) -> number,
    noise: @checked (x: number, y: number?, z: number?) -> number,
    round: @checked (n: number) -> number,
    map: @checked (x: number, inmin: number, inmax: number, outmin: number, outmax: number) -> number,
    lerp: @checked (a: number, b: number, t: number) -> number,

    isnan: @checked (x: number) -> boolean,
    isinf: @checked (x: number) -> boolean,
    isfinite: @checked (x: number) -> boolean,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionOsSrc = R"BUILTIN_SRC(

type DateTypeArg = {
    year: number,
    month: number,
    day: number,
    hour: number?,
    min: number?,
    sec: number?,
    isdst: boolean?,
}

type DateTypeResult = {
    year: number,
    month: number,
    wday: number,
    yday: number,
    day: number,
    hour: number,
    min: number,
    sec: number,
    isdst: boolean,
}

declare os: {
    time: (time: DateTypeArg?) -> number,
    date: ((formatString: "*t" | "!*t", time: number?) -> DateTypeResult) & ((formatString: string?, time: number?) -> string),
    difftime: (t2: DateTypeResult | number, t1: DateTypeResult | number) -> number,
    clock: () -> number,
    getenv: @checked (varname: string) -> string?,
    setenv: @checked (varname: string, value: string?) -> boolean,
    execute: @checked (command: string?) -> (number?, string?, number?),
    remove: @checked (path: string) -> (boolean, string?, number?),
    rename: @checked (oldpath: string, newpath: string) -> (boolean, string?, number?),
    exit: @checked (code: number?, close: boolean?) -> never,
    tmpname: () -> string,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionCoroutineSrc = R"BUILTIN_SRC(

declare coroutine: {
    create: <A..., R...>(f: (A...) -> R...) -> thread,
    resume: <A..., R...>(co: thread, A...) -> (boolean, R...),
    running: () -> thread,
    status: @checked (co: thread) -> "dead" | "running" | "normal" | "suspended",
    wrap: <A..., R...>(f: (A...) -> R...) -> ((A...) -> R...),
    yield: <A..., R...>(A...) -> R...,
    isyieldable: () -> boolean,
    close: @checked (co: thread) -> (boolean, any)
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionTaskSrc = R"BUILTIN_SRC(

declare task: {
    spawn: (f: any, ...any) -> thread,
    defer: (f: any, ...any) -> thread,
    delay: (sec: number, f: any, ...any) -> thread,
    wait: (sec: number?) -> number,
    yield: () -> (),
    cancel: (target: any) -> (),
    now: () -> number,
    clock: () -> number,
    sleep: (sec: number?) -> number,
    poll: (timeout: number?) -> number,
    step: (timeout: number?) -> number,
    run: (f: any?) -> boolean,
    stop: () -> (),
    is_running: () -> boolean,
    isrunning: () -> boolean,

    await: (target: any) -> any,
    create: (executor: (resolve: (...any) -> (), reject: (any) -> ()) -> ()) -> any,
    promise: (executor: (resolve: (...any) -> (), reject: (any) -> ()) -> ()) -> any,
    resolve: (...any) -> any,
    reject: (any) -> any,
    async: (f: any) -> any,
    all: (list: {any}) -> any,
    race: (list: {any}) -> any,
    any: (list: {any}) -> any,
    allSettled: (list: {any}) -> any,

    channel: (capacity: number?) -> any,
    timer: (interval: number, callback: () -> (), repeating: boolean?) -> any,
    poll_read: (sock: any, timeout: number?) -> (boolean, boolean),
    poll_write: (sock: any, timeout: number?) -> (boolean, boolean),
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionTableSrc = R"BUILTIN_SRC(

declare table: {
    concat: <V>(t: {V}, sep: string?, i: number?, j: number?) -> string,
    insert: (<V>(t: {V}, value: V) -> ()) & (<V>(t: {V}, pos: number, value: V) -> ()),
    maxn: <V>(t: {V}) -> number,
    remove: <V>(t: {V}, number?) -> V?,
    sort: <V>(t: {V}, comp: ((V, V) -> boolean)?) -> (),
    create: <V>(count: number, value: V?) -> {V},
    find: <V>(haystack: {V}, needle: V, init: number?) -> number?,

    unpack: <V>(list: {V}, i: number?, j: number?) -> ...V,
    pack: <V>(...V) -> { n: number, [number]: V },

    getn: <V>(t: {V}) -> number,
    foreach: <K, V>(t: {[K]: V}, f: (K, V) -> ()) -> (),
    foreachi: <V>({V}, (number, V) -> ()) -> (),

    move: <V>(src: {V}, a: number, b: number, t: number, dst: {V}?) -> {V},

    clear: (table: {}) -> (),
    isfrozen: (t: {}) -> boolean,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionDebugSrc = R"BUILTIN_SRC(

declare debug: {
    info: ((thread: thread, level: number, options: string) -> ...any) & ((level: number, options: string) -> ...any) & (<A..., R1...>(func: (A...) -> R1..., options: string) -> ...any),
    traceback: ((message: string?, level: number?) -> string) & ((thread: thread, message: string?, level: number?) -> string),
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionUtf8Src = R"BUILTIN_SRC(

declare utf8: {
    char: @checked (...number) -> string,
    charpattern: string,
    codes: @checked (str: string) -> ((string, number) -> (number, number), string, number),
    codepoint: @checked (str: string, i: number?, j: number?) -> ...number,
    len: @checked (s: string, i: number?, j: number?) -> (number?, number?),
    offset: @checked (s: string, n: number?, i: number?) -> number,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionBufferSrc = R"BUILTIN_SRC(
--- Buffer API
declare buffer: {
    create: @checked (size: number) -> buffer,
    fromstring: @checked (str: string) -> buffer,
    tostring: @checked (b: buffer) -> string,
    len: @checked (b: buffer) -> number,
    copy: @checked (target: buffer, targetOffset: number, source: buffer, sourceOffset: number?, count: number?) -> (),
    fill: @checked (b: buffer, offset: number, value: number, count: number?) -> (),
    readi8: @checked (b: buffer, offset: number) -> number,
    readu8: @checked (b: buffer, offset: number) -> number,
    readi16: @checked (b: buffer, offset: number) -> number,
    readu16: @checked (b: buffer, offset: number) -> number,
    readi32: @checked (b: buffer, offset: number) -> number,
    readu32: @checked (b: buffer, offset: number) -> number,
    readf32: @checked (b: buffer, offset: number) -> number,
    readf64: @checked (b: buffer, offset: number) -> number,
    writei8: @checked (b: buffer, offset: number, value: number) -> (),
    writeu8: @checked (b: buffer, offset: number, value: number) -> (),
    writei16: @checked (b: buffer, offset: number, value: number) -> (),
    writeu16: @checked (b: buffer, offset: number, value: number) -> (),
    writei32: @checked (b: buffer, offset: number, value: number) -> (),
    writeu32: @checked (b: buffer, offset: number, value: number) -> (),
    writef32: @checked (b: buffer, offset: number, value: number) -> (),
    writef64: @checked (b: buffer, offset: number, value: number) -> (),
    readstring: @checked (b: buffer, offset: number, count: number) -> string,
    writestring: @checked (b: buffer, offset: number, value: string, count: number?) -> (),
    readbits: @checked (b: buffer, bitOffset: number, bitCount: number) -> number,
    writebits: @checked (b: buffer, bitOffset: number, bitCount: number, value: number) -> (),
    readinteger: @checked (b: buffer, offset: number) -> integer,
    writeinteger: @checked (b: buffer, offset: number, value: integer) -> (),
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionBufferSrc_NOINTEGER = R"BUILTIN_SRC(
--- Buffer API
declare buffer: {
    create: @checked (size: number) -> buffer,
    fromstring: @checked (str: string) -> buffer,
    tostring: @checked (b: buffer) -> string,
    len: @checked (b: buffer) -> number,
    copy: @checked (target: buffer, targetOffset: number, source: buffer, sourceOffset: number?, count: number?) -> (),
    fill: @checked (b: buffer, offset: number, value: number, count: number?) -> (),
    readi8: @checked (b: buffer, offset: number) -> number,
    readu8: @checked (b: buffer, offset: number) -> number,
    readi16: @checked (b: buffer, offset: number) -> number,
    readu16: @checked (b: buffer, offset: number) -> number,
    readi32: @checked (b: buffer, offset: number) -> number,
    readu32: @checked (b: buffer, offset: number) -> number,
    readf32: @checked (b: buffer, offset: number) -> number,
    readf64: @checked (b: buffer, offset: number) -> number,
    writei8: @checked (b: buffer, offset: number, value: number) -> (),
    writeu8: @checked (b: buffer, offset: number, value: number) -> (),
    writei16: @checked (b: buffer, offset: number, value: number) -> (),
    writeu16: @checked (b: buffer, offset: number, value: number) -> (),
    writei32: @checked (b: buffer, offset: number, value: number) -> (),
    writeu32: @checked (b: buffer, offset: number, value: number) -> (),
    writef32: @checked (b: buffer, offset: number, value: number) -> (),
    writef64: @checked (b: buffer, offset: number, value: number) -> (),
    readstring: @checked (b: buffer, offset: number, count: number) -> string,
    writestring: @checked (b: buffer, offset: number, value: string, count: number?) -> (),
    readbits: @checked (b: buffer, bitOffset: number, bitCount: number) -> number,
    writebits: @checked (b: buffer, bitOffset: number, bitCount: number, value: number) -> ()
}

)BUILTIN_SRC";

static const char* const kBuiltinDefinitionVectorSrc = R"BUILTIN_SRC(

-- While vector would have been better represented as a built-in primitive type, type solver extern type handling covers most of the properties
declare extern type vector with
    read x: number
    read y: number
    read z: number
end

declare vector: {
    create: @checked (x: number, y: number, z: number?) -> vector,
    magnitude: @checked (vec: vector) -> number,
    normalize: @checked (vec: vector) -> vector,
    cross: @checked (vec1: vector, vec2: vector) -> vector,
    dot: @checked (vec1: vector, vec2: vector) -> number,
    angle: @checked (vec1: vector, vec2: vector, axis: vector?) -> number,
    floor: @checked (vec: vector) -> vector,
    ceil: @checked (vec: vector) -> vector,
    abs: @checked (vec: vector) -> vector,
    sign: @checked (vec: vector) -> vector,
    clamp: @checked (vec: vector, min: vector, max: vector) -> vector,
    max: @checked (vector, ...vector) -> vector,
    min: @checked (vector, ...vector) -> vector,
    lerp: @checked (vec1: vector, vec2: vector, t: number) -> vector,

    zero: vector,
    one: vector,
}

)BUILTIN_SRC";

static const char* const kBuiltinDefinitionIntegerSrc = R"BUILTIN_SRC(

declare integer: {
    create: @checked (x: number) -> integer,
    tonumber: @checked (x: integer) -> number,
    neg: @checked (value: integer) -> integer,
    add: @checked (x: integer, y: integer) -> integer,
    sub: @checked (x: integer, y: integer) -> integer,
    mul: @checked (x: integer, y: integer) -> integer,
    div: @checked (x: integer, y: integer) -> integer,
    rem: @checked (x: integer, y: integer) -> integer,
    idiv: @checked (x: integer, y: integer) -> integer,
    mod: @checked (x: integer, y: integer) -> integer,
    udiv: @checked (x: integer, y: integer) -> integer,
    urem: @checked (x: integer, y: integer) -> integer,
    min: @checked (integer, ...integer) -> integer,
    max: @checked (integer, ...integer) -> integer,
    band: @checked (...integer) -> integer,
    bor: @checked (...integer) -> integer,
    bnot: @checked (x: integer) -> integer,
    bxor: @checked (...integer) -> integer,
    lt: @checked (x: integer, y: integer) -> boolean,
    le: @checked (x: integer, y: integer) -> boolean,
    ult: @checked (x: integer, y: integer) -> boolean,
    ule: @checked (x: integer, y: integer) -> boolean,
    gt: @checked (x: integer, y: integer) -> boolean,
    ge: @checked (x: integer, y: integer) -> boolean,
    ugt: @checked (x: integer, y: integer) -> boolean,
    uge: @checked (x: integer, y: integer) -> boolean,
    lshift: @checked (x: integer, numBitPositions: integer) -> integer,
    rshift: @checked (x: integer, numBitPositions: integer) -> integer,
    arshift: @checked (x: integer, numBitPositions: integer) -> integer,
    lrotate: @checked (x: integer, numBitPositions: integer) -> integer,
    rrotate: @checked (x: integer, numBitPositions: integer) -> integer,
    extract: @checked (value: integer, bitPosition: integer, numBits: integer?) -> integer,
    replace: @checked (value: integer, replacement: integer, bitPosition: integer, numBits: integer?) -> integer,
    clamp: @checked (value: integer, min: integer, max: integer) -> integer,
    btest: @checked (...integer) -> boolean,
    countrz: @checked (x: integer) -> integer,
    countlz: @checked (x: integer) -> integer,
    bswap: @checked (x: integer) -> integer,
    fromstring: @checked (str: string, base: number?) -> integer,
    minsigned: integer,
    maxsigned: integer
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionFsSrc = R"BUILTIN_SRC(

export type FileStat = {
    exists: boolean,
    isFile: boolean,
    isDirectory: boolean,
    size: number,
    modified: number,
}

declare fs: {
    readfile: @checked (path: string) -> string,
    writefile: @checked (path: string, contents: string | buffer) -> (),
    appendfile: @checked (path: string, contents: string | buffer) -> (),
    removefile: @checked (path: string) -> (),
    removedir: @checked (path: string, recursive: boolean?) -> (),
    mkdir: @checked (path: string, recursive: boolean?) -> (),
    list: @checked (path: string) -> {string},
    isfile: @checked (path: string) -> boolean,
    isdir: @checked (path: string) -> boolean,
    exists: @checked (path: string) -> boolean,
    stat: @checked (path: string) -> FileStat?,
    copy: @checked (from: string, to: string, overwrite: boolean?) -> (),
    move: @checked (from: string, to: string) -> (),
    cwd: () -> string,

    readFile: @checked (path: string) -> string,
    writeFile: @checked (path: string, contents: string | buffer) -> (),
    appendFile: @checked (path: string, contents: string | buffer) -> (),
    removeFile: @checked (path: string) -> (),
    removeDir: @checked (path: string, recursive: boolean?) -> (),
    makeDir: @checked (path: string, recursive: boolean?) -> (),
    readDir: @checked (path: string) -> {string},
    isFile: @checked (path: string) -> boolean,
    isDir: @checked (path: string) -> boolean,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionIoSrc = R"BUILTIN_SRC(

export type FileHandle = {
    close: (self: FileHandle) -> (boolean, string?, number?),
    flush: (self: FileHandle) -> (boolean, string?),
    lines: (self: FileHandle) -> () -> string?,
    read: (self: FileHandle, ...string | number) -> ...any,
    seek: (self: FileHandle, whence: ("set" | "cur" | "end")?, offset: number?) -> (number?, string?),
    write: (self: FileHandle, ...string | number | buffer) -> (FileHandle | boolean, string?),
}

declare io: {
    close: (file: FileHandle?) -> (boolean, string?, number?),
    flush: () -> (boolean, string?),
    input: ((file: FileHandle | string) -> FileHandle) & (() -> FileHandle),
    output: ((file: FileHandle | string) -> FileHandle) & (() -> FileHandle),
    lines: (filename: string?) -> () -> string?,
    open: @checked (filename: string, mode: string?) -> (FileHandle?, string?),
    popen: @checked (command: string, mode: string?) -> (FileHandle?, string?),
    read: (...string | number) -> ...any,
    tmpfile: () -> (FileHandle?, string?),
    type: (obj: any) -> "file" | "closed file" | nil,
    write: (...string | number | buffer) -> (boolean, string?),
    stdin: FileHandle,
    stdout: FileHandle,
    stderr: FileHandle,
}

)BUILTIN_SRC";

static const char* kBuiltinDefinitionClassSrc = R"CLASS_SRC(
declare class: {
    isinstance: @checked (o: unknown, c: class) -> boolean,
    classof: @checked (o: unknown) -> class?
}
)CLASS_SRC";

static constexpr const char* kBuiltinDefinitionJniSrc = R"BUILTIN_SRC(
declare jni: {
    init: @checked (config: { classpath: (string | {string})?, options: {string}?, jvm_path: string?, ignore_unrecognized: boolean? }?) -> boolean,
    is_initialized: @checked () -> boolean,
    isInitialized: @checked () -> boolean,
    destroy: @checked () -> boolean,
    get_version: @checked () -> string,
    getVersion: @checked () -> string,
    find_jvm_path: @checked () -> string,
    findJvmPath: @checked () -> string,
    attach_current_thread: @checked () -> boolean,
    attachCurrentThread: @checked () -> boolean,
    detach_current_thread: @checked () -> boolean,
    detachCurrentThread: @checked () -> boolean,
    with_local_frame: @checked (capacityOrFn: number | (() -> ()), fn: (() -> ())?) -> ...any,
    withLocalFrame: @checked (capacityOrFn: number | (() -> ()), fn: (() -> ())?) -> ...any,
    local_frame: @checked (capacityOrFn: number | (() -> ()), fn: (() -> ())?) -> ...any,
    localFrame: @checked (capacityOrFn: number | (() -> ()), fn: (() -> ())?) -> ...any,
    find_class: @checked (className: string) -> any,
    findClass: @checked (className: string) -> any,
    class: @checked (className: string) -> any,
    import: @checked (className: string) -> any,
    new: @checked (classNameOrClass: any, ...any) -> any,
    array: @checked (typeName: string, sizeOrTable: number | {any}) -> any,
    wrap_buffer: @checked (buf: buffer) -> any,
    wrapBuffer: @checked (buf: buffer) -> any,
    to_java: @checked (val: any) -> any,
    toJava: @checked (val: any) -> any,
    to_luau: @checked (obj: any) -> any,
    toLuau: @checked (obj: any) -> any,
    instanceof: @checked (obj: any, classNameOrClass: any) -> boolean,
    instanceOf: @checked (obj: any, classNameOrClass: any) -> boolean,
    cast: @checked (obj: any, classNameOrClass: any) -> any,
    call_method: @checked (obj: any, methodName: string, ...any) -> any,
    callMethod: @checked (obj: any, methodName: string, ...any) -> any,
    call_static: @checked (classOrName: any, methodName: string, ...any) -> any,
    callStatic: @checked (classOrName: any, methodName: string, ...any) -> any,
    jboolean: @checked (val: boolean) -> any,
    jbyte: @checked (val: number) -> any,
    jchar: @checked (val: string | number) -> any,
    jshort: @checked (val: number) -> any,
    jint: @checked (val: number) -> any,
    jlong: @checked (val: number) -> any,
    jfloat: @checked (val: number) -> any,
    jdouble: @checked (val: number) -> any,
    jstring: @checked (val: string) -> any,
    null: any,
}
)BUILTIN_SRC";

std::string getBuiltinDefinitionSource()
{
    std::string result = kBuiltinDefinitionBaseSrc;

    result += kBuiltinDefinitionBit32Src;
    result += kBuiltinDefinitionMathSrc;
    result += kBuiltinDefinitionOsSrc;
    result += kBuiltinDefinitionFsSrc;
    result += kBuiltinDefinitionIoSrc;
    result += kBuiltinDefinitionCoroutineSrc;
    result += kBuiltinDefinitionTaskSrc;
    result += kBuiltinDefinitionTableSrc;
    result += kBuiltinDefinitionDebugSrc;
    result += kBuiltinDefinitionUtf8Src;
    if (FFlag::LuauIntegerType2 && FFlag::LuauIntegerLibrary)
        result += kBuiltinDefinitionBufferSrc;
    else
        result += kBuiltinDefinitionBufferSrc_NOINTEGER;

    result += kBuiltinDefinitionVectorSrc;

    if (FFlag::LuauIntegerType2 && FFlag::LuauIntegerLibrary)
    {
        result += kBuiltinDefinitionIntegerSrc;
    }

    if (FFlag::DebugLuauUserDefinedClasses && FFlag::LuauAllowGlobalDeclarationToBeCalledClass)
    {
        result += kBuiltinDefinitionClassSrc;
    }

    result += kBuiltinDefinitionJniSrc;

    return result;
}

// TODO: split into separate tagged unions when the new solver can appropriately handle that.
static constexpr const char* kBuiltinDefinitionTypeMethodSrc = R"BUILTIN_SRC(

export type type = {
    tag: "nil" | "unknown" | "never" | "any" | "boolean" | "number" | "integer" | "string" | "buffer" | "thread" |
         "singleton" | "negation" | "union" | "intersection" | "table" | "function" | "extern" | "generic",

    is: (self: type, arg: string) -> boolean,
    issubtypeof: (self: type, arg: type) -> boolean,

    -- for singleton type
    value: (self: type) -> (string | boolean | nil),

    -- for negation type
    inner: (self: type) -> type,

    -- for union and intersection types
    components: (self: type) -> {type},

    -- for table type
    setproperty: (self: type, key: type, value: type?) -> (),
    setreadproperty: (self: type, key: type, value: type?) -> (),
    setwriteproperty: (self: type, key: type, value: type?) -> (),
    readproperty: (self: type, key: type) -> type?,
    writeproperty: (self: type, key: type) -> type?,
    properties: (self: type) -> { [type]: { read: type?, write: type? } },
    setindexer: (self: type, index: type, result: type) -> (),
    setreadindexer: (self: type, index: type, result: type) -> (),
    setwriteindexer: (self: type, index: type, result: type) -> (),
    indexer: (self: type) -> { index: type, readresult: type, writeresult: type }?,
    readindexer: (self: type) -> { index: type, result: type }?,
    writeindexer: (self: type) -> { index: type, result: type }?,
    setmetatable: (self: type, arg: type) -> (),
    metatable: (self: type) -> type?,

    -- for function type
    setparameters: (self: type, head: {type}?, tail: type?) -> (),
    parameters: (self: type) -> { head: {type}?, tail: type? },
    setreturns: (self: type, head: {type}?, tail: type? ) -> (),
    returns: (self: type) -> { head: {type}?, tail: type? },
    setgenerics: (self: type, {type}?) -> (),
    generics: (self: type) -> {type},

    -- for class type
    -- 'properties', 'metatable', 'indexer', 'readindexer' and 'writeindexer' are shared with table type
    readparent: (self: type) -> type?,
    writeparent: (self: type) -> type?,

    -- for generic type
    name: (self: type) -> string?,
    ispack: (self: type) -> boolean,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionTypeMethodSrc_NOINTEGER = R"BUILTIN_SRC(

export type type = {
    tag: "nil" | "unknown" | "never" | "any" | "boolean" | "number" | "string" | "buffer" | "thread" |
         "singleton" | "negation" | "union" | "intersection" | "table" | "function" | "extern" | "generic",

    is: (self: type, arg: string) -> boolean,
    issubtypeof: (self: type, arg: type) -> boolean,

    -- for singleton type
    value: (self: type) -> (string | boolean | nil),

    -- for negation type
    inner: (self: type) -> type,

    -- for union and intersection types
    components: (self: type) -> {type},

    -- for table type
    setproperty: (self: type, key: type, value: type?) -> (),
    setreadproperty: (self: type, key: type, value: type?) -> (),
    setwriteproperty: (self: type, key: type, value: type?) -> (),
    readproperty: (self: type, key: type) -> type?,
    writeproperty: (self: type, key: type) -> type?,
    properties: (self: type) -> { [type]: { read: type?, write: type? } },
    setindexer: (self: type, index: type, result: type) -> (),
    setreadindexer: (self: type, index: type, result: type) -> (),
    setwriteindexer: (self: type, index: type, result: type) -> (),
    indexer: (self: type) -> { index: type, readresult: type, writeresult: type }?,
    readindexer: (self: type) -> { index: type, result: type }?,
    writeindexer: (self: type) -> { index: type, result: type }?,
    setmetatable: (self: type, arg: type) -> (),
    metatable: (self: type) -> type?,

    -- for function type
    setparameters: (self: type, head: {type}?, tail: type?) -> (),
    parameters: (self: type) -> { head: {type}?, tail: type? },
    setreturns: (self: type, head: {type}?, tail: type? ) -> (),
    returns: (self: type) -> { head: {type}?, tail: type? },
    setgenerics: (self: type, {type}?) -> (),
    generics: (self: type) -> {type},

    -- for class type
    -- 'properties', 'metatable', 'indexer', 'readindexer' and 'writeindexer' are shared with table type
    readparent: (self: type) -> type?,
    writeparent: (self: type) -> type?,

    -- for generic type
    name: (self: type) -> string?,
    ispack: (self: type) -> boolean,
}

)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionTypesLibSrc = R"BUILTIN_SRC(

declare types: {
    unknown: type,
    never: type,
    any: type,
    boolean: type,
    number: type,
    string: type,
    thread: type,
    buffer: type,
    integer: type,

    singleton: @checked (arg: string | boolean | nil) -> type,
    optional: @checked (arg: type) -> type,
    generic: @checked (name: string, ispack: boolean?) -> type,
    negationof: @checked (arg: type) -> type,
    unionof: @checked (...type) -> type,
    intersectionof: @checked (...type) -> type,
    newtable: @checked (props: {[type]: type} | {[type]: { read: type?, write: type? } }?, indexer: { index: type, readresult: type, writeresult: type }?, metatable: type?) -> type,
    newfunction: @checked (parameters: { head: {type}?, tail: type? }?, returns: { head: {type}?, tail: type? }?, generics: {type}?) -> type,
    copy: @checked (arg: type) -> type,
}
)BUILTIN_SRC";

static constexpr const char* kBuiltinDefinitionTypesLibSrc_NOINTEGER = R"BUILTIN_SRC(

declare types: {
    unknown: type,
    never: type,
    any: type,
    boolean: type,
    number: type,
    string: type,
    thread: type,
    buffer: type,

    singleton: @checked (arg: string | boolean | nil) -> type,
    optional: @checked (arg: type) -> type,
    generic: @checked (name: string, ispack: boolean?) -> type,
    negationof: @checked (arg: type) -> type,
    unionof: @checked (...type) -> type,
    intersectionof: @checked (...type) -> type,
    newtable: @checked (props: {[type]: type} | {[type]: { read: type?, write: type? } }?, indexer: { index: type, readresult: type, writeresult: type }?, metatable: type?) -> type,
    newfunction: @checked (parameters: { head: {type}?, tail: type? }?, returns: { head: {type}?, tail: type? }?, generics: {type}?) -> type,
    copy: @checked (arg: type) -> type,
}
)BUILTIN_SRC";

std::string getTypeFunctionDefinitionSource()
{
    std::string result;

    if (FFlag::LuauIntegerType2)
        result += kBuiltinDefinitionTypeMethodSrc;
    else
        result += kBuiltinDefinitionTypeMethodSrc_NOINTEGER;

    if (FFlag::LuauIntegerType2)
        result += kBuiltinDefinitionTypesLibSrc;
    else
        result += kBuiltinDefinitionTypesLibSrc_NOINTEGER;

    return result;
}

} // namespace Luau
