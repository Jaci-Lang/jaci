// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"
#include "Luau/FileUtils.h"

#include "doctest.h"

#include <memory>
#include <string>
#include <filesystem>

namespace fs_std = std::filesystem;

class FilesystemFixture
{
public:
    FilesystemFixture()
        : state(luaL_newstate(), lua_close)
    {
        L = state.get();
        luaL_openlibs(L);
    }

    std::string run(const std::string& code)
    {
        std::string bytecode = Luau::compile(code);
        if (luau_load(L, "=test", bytecode.data(), bytecode.size(), 0) != 0)
        {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }

        int status = lua_pcall(L, 0, 0, 0);
        if (status != 0)
        {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }

        return "";
    }

    lua_State* L;

private:
    std::unique_ptr<lua_State, void (*)(lua_State*)> state;
};

TEST_SUITE_BEGIN("FilesystemTests");

TEST_CASE_FIXTURE(FilesystemFixture, "FsWriteReadRemove")
{
    std::string err = run(R"(
        local testPath = "test_jaci_temp.txt"
        fs.writefile(testPath, "Hello Jaci Filesystem!")
        assert(fs.exists(testPath), "file should exist")
        assert(fs.isfile(testPath), "should be a file")
        assert(not fs.isdir(testPath), "should not be a directory")

        local content = fs.readfile(testPath)
        assert(content == "Hello Jaci Filesystem!", "content mismatch: " .. tostring(content))

        fs.appendfile(testPath, " More data.")
        assert(fs.readfile(testPath) == "Hello Jaci Filesystem! More data.")

        local stat = fs.stat(testPath)
        assert(stat ~= nil, "stat should not be nil")
        assert(stat.exists == true, "stat exists")
        assert(stat.isFile == true, "stat isFile")
        assert(stat.isDirectory == false, "stat isDirectory")
        assert(stat.size > 0, "stat size > 0")

        fs.removefile(testPath)
        assert(not fs.exists(testPath), "file should be removed")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FilesystemFixture, "FsBufferSupport")
{
    std::string err = run(R"(
        local testPath = "test_jaci_buf.bin"
        local b = buffer.create(4)
        buffer.writeu8(b, 0, 65) -- 'A'
        buffer.writeu8(b, 1, 66) -- 'B'
        buffer.writeu8(b, 2, 67) -- 'C'
        buffer.writeu8(b, 3, 68) -- 'D'

        fs.writefile(testPath, b)
        assert(fs.readfile(testPath) == "ABCD", "buffer write mismatch")

        local b2 = buffer.create(2)
        buffer.writeu8(b2, 0, 69) -- 'E'
        buffer.writeu8(b2, 1, 70) -- 'F'
        fs.appendfile(testPath, b2)
        assert(fs.readfile(testPath) == "ABCDEF", "buffer append mismatch")

        fs.removefile(testPath)
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FilesystemFixture, "FsDirectoriesAndListing")
{
    std::string err = run(R"(
        local dirPath = "test_jaci_dir/sub"
        fs.mkdir(dirPath, true)
        assert(fs.exists(dirPath), "dir should exist")
        assert(fs.isdir(dirPath), "should be a directory")
        assert(not fs.isfile(dirPath), "should not be a file")

        local filePath = dirPath .. "/file.txt"
        fs.writefile(filePath, "sample")

        local entries = fs.list(dirPath)
        assert(#entries == 1, "expected 1 entry, got " .. #entries)
        assert(entries[1] == "file.txt", "entry name mismatch")

        fs.removefile(filePath)
        fs.removedir("test_jaci_dir", true)
        assert(not fs.exists("test_jaci_dir"), "directory should be removed")
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FilesystemFixture, "FsAliasesAndCwd")
{
    std::string err = run(R"(
        local cwd = fs.cwd()
        assert(type(cwd) == "string" and #cwd > 0, "invalid cwd")

        local testPath = "test_jaci_aliases.txt"
        fs.writeFile(testPath, "aliased content")
        assert(fs.isFile(testPath), "isFile alias")
        assert(fs.readFile(testPath) == "aliased content", "readFile alias")

        fs.appendFile(testPath, " extra")
        assert(fs.readFile(testPath) == "aliased content extra")

        local copyPath = "test_jaci_copy.txt"
        fs.copy(testPath, copyPath)
        assert(fs.readFile(copyPath) == "aliased content extra")

        local movePath = "test_jaci_moved.txt"
        fs.move(copyPath, movePath)
        assert(not fs.exists(copyPath))
        assert(fs.readFile(movePath) == "aliased content extra")

        fs.removeFile(testPath)
        fs.removeFile(movePath)
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FilesystemFixture, "IoLibraryBasicStream")
{
    std::string err = run(R"(
        local testPath = "test_jaci_io.txt"
        local f = io.open(testPath, "w")
        assert(f ~= nil, "io.open write should succeed")
        assert(io.type(f) == "file", "type should be file")
        f:write("line 1\nline 2\n12345\n")
        f:flush()
        f:close()

        assert(io.type(f) == "closed file", "type should be closed file")

        local rf = io.open(testPath, "r")
        assert(rf ~= nil, "io.open read should succeed")
        local l1 = rf:read("*l")
        assert(l1 == "line 1", "line 1 mismatch: " .. tostring(l1))
        local l2 = rf:read("*line")
        assert(l2 == "line 2", "line 2 mismatch: " .. tostring(l2))
        local num = rf:read("*n")
        assert(num == 12345, "number mismatch: " .. tostring(num))
        rf:close()

        os.remove(testPath)
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FilesystemFixture, "IoLinesIterator")
{
    std::string err = run(R"(
        local testPath = "test_jaci_lines.txt"
        local f = io.open(testPath, "w")
        f:write("alpha\nbeta\ngamma\n")
        f:close()

        local lines = {}
        for l in io.lines(testPath) do
            table.insert(lines, l)
        end

        assert(#lines == 3, "expected 3 lines")
        assert(lines[1] == "alpha")
        assert(lines[2] == "beta")
        assert(lines[3] == "gamma")

        os.remove(testPath)
    )");
    CHECK(err == "");
}

TEST_CASE_FIXTURE(FilesystemFixture, "OsEnvironmentAndExecute")
{
    std::string err = run(R"(
        local key = "JACI_TEST_VAR_123"
        os.setenv(key, "JACI_RUNNING")
        assert(os.getenv(key) == "JACI_RUNNING", "getenv mismatch")

        os.setenv(key, nil)
        assert(os.getenv(key) == nil, "getenv after unset mismatch")

        local tmp = os.tmpname()
        assert(type(tmp) == "string" and #tmp > 0, "tmpname invalid")

        local status = os.execute("true")
        assert(status == 0 or status == true, "execute true failed")
    )");
    CHECK(err == "");
}

TEST_CASE("ResolveSymlink")
{
    // Create a temp file and a symlink to it, then verify resolveSymlink returns the real path.
    std::string target = "test_resolve_symlink_target.txt";
    std::string link = "test_resolve_symlink_link.txt";

    FILE* fp = fopen(target.c_str(), "w");
    CHECK(fp != nullptr);
    if (fp)
    {
        fputs("hello", fp);
        fclose(fp);
    }

    // Create symlink (POSIX)
#ifdef _WIN32
    // Windows: use CreateSymbolicLinkW - skip in unit test for simplicity
#else
    int ret = symlink(target.c_str(), link.c_str());
    CHECK(ret == 0);
    CHECK(fs_std::exists(link));
#endif

    // resolveSymlink should return the canonical (real) path
    auto resolved = Luau::resolveSymlink(link);
#ifdef _WIN32
    // Windows path not tested in this unit test
#else
    CHECK(resolved.has_value());
    if (resolved.has_value())
    {
        // The resolved path should equal the real path of the target
        auto targetReal = Luau::resolveSymlink(target);
        CHECK(targetReal.has_value());
        CHECK(*resolved == *targetReal);
    }
#endif

    // Cleanup
    remove(link.c_str());
    remove(target.c_str());
}

TEST_CASE("ResolveSymlinkChain")
{
    // Test that resolveSymlink follows a chain of symlinks.
    std::string target = "test_chain_target.txt";
    std::string link1 = "test_chain_link1.txt";
    std::string link2 = "test_chain_link2.txt";

    FILE* fp = fopen(target.c_str(), "w");
    CHECK(fp != nullptr);
    if (fp)
    {
        fputs("data", fp);
        fclose(fp);
    }

#ifdef _WIN32
    // Windows: skip
#else
    // target <- link1 <- link2
    CHECK(symlink(target.c_str(), link1.c_str()) == 0);
    CHECK(symlink(link1.c_str(), link2.c_str()) == 0);

    auto resolved = Luau::resolveSymlink(link2);
    CHECK(resolved.has_value());
    if (resolved.has_value())
    {
        auto targetReal = Luau::resolveSymlink(target);
        CHECK(targetReal.has_value());
        CHECK(*resolved == *targetReal);
    }

    remove(link2.c_str());
    remove(link1.c_str());
#endif
    remove(target.c_str());
}

TEST_SUITE_END();
