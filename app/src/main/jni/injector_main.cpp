#include "injector.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *target = "cameraserver";
    const char *loader = "/data/camera/libhook.so";
    const char *export_symbol = nullptr;
    int positional = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s [target_process] [payload_so_path] [export_symbol]\n", argv[0]);
            return 1;
        }
        if (positional == 0) {
            target = argv[i];
            positional += 1;
        } else if (positional == 1) {
            loader = argv[i];
            positional += 1;
        } else if (positional == 2) {
            export_symbol = argv[i];
            positional += 1;
        }
    }

    return do_inject(target, loader, export_symbol);
}
