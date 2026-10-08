// Survey Xenos shader microcode dumped by NFSMW_DUMP_SHADERS: which control
// flow ops, ALU ops, fetches and exports the game's shaders actually use.
//   xenos_survey game/shaders/*.bin
#include "../runtime/gpu/xenos/ucode.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace xe::gpu::ucode;

int main(int argc, char** argv)
{
    std::map<std::string, int> cf, vec, sca, fetch, exports;
    for (int a = 1; a < argc; a++)
    {
        FILE* f = fopen(argv[a], "rb");
        std::vector<uint32_t> w;
        uint32_t v;
        while (fread(&v, 4, 1, f) == 1)
            w.push_back(__builtin_bswap32(v));
        fclose(f);
        bool isPs = std::string(argv[a]).find("ps_") != std::string::npos;

        uint32_t bound = uint32_t(w.size() / 3);
        for (uint32_t i = 0; i < bound; i++)
        {
            ControlFlowInstruction ab[2];
            UnpackControlFlowInstructions(w.data() + i * 3, ab);
            for (auto& c : ab)
                if (IsControlFlowOpcodeExec(c.opcode()))
                    bound = std::min(bound, c.exec.address());
        }
        for (uint32_t i = 0; i < bound; i++)
        {
            ControlFlowInstruction ab[2];
            UnpackControlFlowInstructions(w.data() + i * 3, ab);
            for (auto& c : ab)
            {
                cf["cf " + std::to_string(uint32_t(c.opcode()))]++;
                if (!IsControlFlowOpcodeExec(c.opcode()))
                    continue;
                uint32_t seq = c.exec.sequence();
                for (uint32_t k = 0; k < c.exec.count(); k++, seq >>= 2)
                {
                    const uint32_t* op = w.data() + (c.exec.address() + k) * 3;
                    if (seq & 1)
                    {
                        auto& fi = *reinterpret_cast<const FetchInstruction*>(op);
                        if (fi.opcode() == FetchOpcode::kVertexFetch)
                            fetch["vfetch fmt " + std::to_string(uint32_t(fi.vertex_fetch().data_format())) +
                                (fi.vertex_fetch().is_mini_fetch() ? " mini" : "")]++;
                        else
                            fetch["tfetch op " + std::to_string(uint32_t(fi.opcode())) + " dim " +
                                std::to_string(uint32_t(fi.texture_fetch().dimension()))]++;
                    }
                    else
                    {
                        auto& al = *reinterpret_cast<const AluInstruction*>(op);
                        vec[std::string(isPs ? "ps" : "vs") + " vec " + std::to_string(uint32_t(al.vector_opcode()))]++;
                        sca[std::string(isPs ? "ps" : "vs") + " sca " + std::to_string(uint32_t(al.scalar_opcode()))]++;
                        if (al.is_export())
                            exports[std::string(isPs ? "ps" : "vs") + " export " + std::to_string(al.vector_dest())]++;
                    }
                }
            }
        }
    }
    for (auto* m : { &cf, &vec, &sca, &fetch, &exports })
    {
        for (auto& [k, n] : *m)
            printf("%-24s %d\n", k.c_str(), n);
        printf("\n");
    }
}
