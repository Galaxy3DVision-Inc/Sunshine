#pragma once

namespace xlang_bridge_runner {
    /**
     * @brief Hooks into the native sunshine executable loop, bypassing standard streams
     * @param bridge_dll_path The absolute or relative path to the sunshine_bridge.dll
     * @param lrpc_port The dynamic LRPC port proxy allocated by Smoora
     * @return 0 on success
     */
    int Start(const char* bridge_dll_path, int lrpc_port);
}
