#!/usr/bin/env python3
import sys

def main():
    if len(sys.argv) != 5:
        sys.stderr.write(f"Usage: {sys.argv[0]} <input_file> <symbol_name> <out_c> <out_h>\n")
        sys.exit(1)

    in_path, sym_name, out_c_path, out_h_path = sys.argv[1:5]
    with open(in_path, "rb") as f:
        data = f.read()

    with open(out_h_path, "w") as fh:
        fh.write("#pragma once\n#include <stddef.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n")
        fh.write(f"extern const unsigned char {sym_name}[];\n")
        fh.write(f"extern const size_t {sym_name}_size;\n")
        fh.write("#ifdef __cplusplus\n}\n#endif\n")

    with open(out_c_path, "w") as fc:
        fc.write(f"#include <stddef.h>\nconst unsigned char {sym_name}[] = {{")
        for i, b in enumerate(data):
            if i % 16 == 0:
                fc.write("\n   ")
            fc.write(f" 0x{b:02x},")
        if len(data) == 0:
            fc.write(" 0")
        fc.write(f"\n}};\nconst size_t {sym_name}_size = {len(data)};\n")

if __name__ == "__main__":
    main()
