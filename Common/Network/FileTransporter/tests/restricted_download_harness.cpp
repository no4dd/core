/* SPDX-License-Identifier: AGPL-3.0-only */
#include "../src/restricted_download.h"
#include <iostream>

int main(int argc, char** argv)
{
    using namespace NSNetwork::NSFileTransport::RestrictedDownload;
    if (argc == 4 && std::string(argv[1]) == "policy")
        return allowed(argv[2], argv[3]) ? 0 : 1;
    if (argc != 8) return 2;
    Limits limits;
    limits.bytes = std::stoull(argv[5]);
    limits.milliseconds = static_cast<unsigned>(std::stoul(argv[6]));
    const unsigned cancelMs = static_cast<unsigned>(std::stoul(argv[7]));
    const auto start = std::chrono::steady_clock::now();
    const int result = download(argv[2], argv[4], argv[3], [&] {
        if (cancelMs == 1) throw std::runtime_error("cancel callback failure");
        return cancelMs && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(cancelMs);
    }, limits);
    int status;
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) return 3;
    return result;
}
