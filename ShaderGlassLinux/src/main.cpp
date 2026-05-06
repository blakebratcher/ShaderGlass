#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "ShaderGC.h"

// Hello-world binary that exercises ShaderGC link surface. Writes an empty
// temp file and round-trips it through ShaderGC::LoadSource so the static lib
// is genuinely linked (not dropped by --gc-sections).
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    std::printf("ShaderGlass Linux M1 — hello.\n");

    namespace fs = std::filesystem;
    const auto tmp = fs::temp_directory_path() / "shaderglass_m1_empty.slang";
    { std::ofstream f(tmp); }  // touch / truncate

    try
    {
        auto vec = ShaderGC::LoadSource(tmp, false);
        std::printf("LoadSource returned %zu lines.\n", vec.size());
    }
    catch(const std::exception& e)
    {
        std::fprintf(stderr, "LoadSource threw: %s\n", e.what());
        return 1;
    }

    fs::remove(tmp);
    return 0;
}
