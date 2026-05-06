(function () {
    const payloadPath = __PAYLOAD_PATH__;

    function resolveEntry(module, payloadPath, exportName) {
        const payloadName = payloadPath.split("/").pop();
        let entry = null;

        if (module && typeof module.findExportByName === "function") {
            try {
                entry = module.findExportByName(exportName);
            } catch (_) {}
        }

        const candidates = [
            module && module.name ? module.name : null,
            payloadName,
            payloadPath,
        ].filter(Boolean);

        for (const name of candidates) {
            if (entry !== null) break;
            try {
                entry = Module.findExportByName(name, exportName);
            } catch (_) {}
        }

        for (const name of candidates) {
            if (entry !== null) break;
            try {
                entry = Module.getExportByName(name, exportName);
            } catch (_) {}
        }

        if (entry === null && typeof Module.findGlobalExportByName === "function") {
            try {
                entry = Module.findGlobalExportByName(exportName);
            } catch (_) {}
        }

        return entry;
    }

    try {
        const module = Module.load(payloadPath);
        const payloadName = payloadPath.split("/").pop();

        if (payloadName === "libhook.so") {
            const entry = resolveEntry(module, payloadPath, "main_hook");

            if (entry === null) {
                throw new Error("main_hook export not found in " + payloadPath +
                    " module.name=" + (module.name || "(null)") +
                    " base=" + module.base);
            }

            const mainHook = new NativeFunction(entry, "void", []);
            mainHook();
        }

        send({
            type: "loaded",
            path: module.path,
            base: module.base.toString()
        });
    } catch (e) {
        send({
            type: "error",
            message: String(e),
            stack: e && e.stack ? e.stack : ""
        });
    }
})();
