#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <input_file> <symbol_name> <out_c> <out_h>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* in_path = argv[1];
    const char* sym_name = argv[2];
    const char* out_c_path = argv[3];
    const char* out_h_path = argv[4];

    FILE* in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "file_to_c: failed to open %s for reading\n", in_path);
        return EXIT_FAILURE;
    }

    FILE* fc = fopen(out_c_path, "w");
    if (!fc) {
        fprintf(stderr, "file_to_c: failed to open %s for writing\n", out_c_path);
        fclose(in);
        return EXIT_FAILURE;
    }

    FILE* fh = fopen(out_h_path, "w");
    if (!fh) {
        fprintf(stderr, "file_to_c: failed to open %s for writing\n", out_h_path);
        fclose(in);
        fclose(fc);
        return EXIT_FAILURE;
    }

    fprintf(fh, "#pragma once\n#include <stddef.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
    fprintf(fh, "extern const unsigned char %s[];\n", sym_name);
    fprintf(fh, "extern const size_t %s_size;\n", sym_name);
    fprintf(fh, "#ifdef __cplusplus\n}\n#endif\n");
    fclose(fh);

    fprintf(fc, "#include <stddef.h>\nconst unsigned char %s[] = {", sym_name);
    int c;
    size_t count = 0;
    while ((c = fgetc(in)) != EOF) {
        if (count % 16 == 0) {
            fprintf(fc, "\n   ");
        }
        fprintf(fc, " 0x%02x,", (unsigned char)c);
        count++;
    }
    fclose(in);

    if (count == 0) {
        fprintf(fc, " 0");
    }
    fprintf(fc, "\n};\nconst size_t %s_size = %zu;\n", sym_name, count);
    fclose(fc);

    return EXIT_SUCCESS;
}
