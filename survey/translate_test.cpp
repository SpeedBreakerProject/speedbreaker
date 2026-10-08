// Translate dumped shaders (NFSMW_DUMP_SHADERS) and write .vert/.frag files
// for glslangValidator.  translate_test <out-dir> game/shaders/*.bin
#include "../runtime/gpu/shader_translator.h"
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    int failed = 0;
    for (int a = 2; a < argc; a++)
    {
        FILE* f = fopen(argv[a], "rb");
        std::vector<uint32_t> w;
        uint32_t v;
        while (fread(&v, 4, 1, f) == 1)
            w.push_back(__builtin_bswap32(v));
        fclose(f);
        std::string path = argv[a];
        std::string name = path.substr(path.find_last_of('/') + 1);
        bool ps = name.rfind("ps_", 0) == 0;
        auto t = gpu::TranslateShader(ps ? gpu::ShaderStage::Pixel : gpu::ShaderStage::Vertex, w.data(), w.size());
        std::string out = std::string(argv[1]) + "/" + name.substr(0, name.size() - 4) + (ps ? ".frag" : ".vert");
        FILE* o = fopen(out.c_str(), "w");
        fwrite(t.glsl.data(), 1, t.glsl.size(), o);
        fclose(o);
        if (!t.ok)
        {
            printf("%s: TRANSLATION ERROR: %s\n", name.c_str(), t.error.c_str());
            failed++;
        }
    }
    printf("%d translation errors\n", failed);
}
