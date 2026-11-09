"""Generate C++ exhaustive checks against the supplied Python interpreter."""
from pathlib import Path
import importlib.util

def generate_reference_test(build):
    root=Path(__file__).resolve().parent
    spec=importlib.util.spec_from_file_location('reference108',root/'tests/reference108.py')
    reference=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    values=[reference.get_key(reference.WORD(seed),108,reference.security_table).value
            for seed in range(65536)]
    target=Path(build)/'key108_reference_test.cpp'
    target.write_text('#include "Key108.h"\nconstexpr uint16_t expectedKeys[]={\n'+
        ',\n'.join(','.join(f'0x{x:04X}' for x in values[i:i+16]) for i in range(0,65536,16))+
        '\n};\nconstexpr bool compareAllSeeds() {\n'
        'for(uint32_t seed=0;seed<65536;++seed)\n'
        'if(corsaEKey108(uint16_t(seed))!=expectedKeys[seed]) return false;\n'
        'return true; }\nstatic_assert(compareAllSeeds(),"Algorithm 108 differs from supplied Python reference");\n')
    print('Generated exhaustive C++ comparison for all 65,536 seeds.',flush=True)
    return target

if __name__=='__main__':
    build=Path(__file__).resolve().parent/'build'
    build.mkdir(exist_ok=True)
    generate_reference_test(build)
