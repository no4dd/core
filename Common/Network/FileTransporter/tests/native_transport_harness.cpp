/* SPDX-License-Identifier: AGPL-3.0-only */
#include "../include/FileTransporter.h"
#include "../../../../DesktopEditor/common/ProcessEnv.h"
#include <string>

int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    NSProcessEnv::Load(std::string("<options><allowNetworkRequest>false</allowNetworkRequest>") +
        "<allowPrivateIP>false</allowPrivateIP><allowList>" + argv[3] + "</allowList></options>");
    const std::string operation(argv[1]), url(argv[2]), file(argv[4]);
    const std::wstring wideUrl(url.begin(), url.end()), wideFile(file.begin(), file.end());
    if (operation == "upload") {
        const unsigned char data[] = "test";
        NSNetwork::NSFileTransport::CFileUploader upload(wideUrl, data, 4);
        return upload.UploadSync() ? 0 : 1;
    }
    if (operation == "upload-file") {
        NSNetwork::NSFileTransport::CFileUploader upload(wideUrl, wideFile);
        return upload.UploadSync() ? 0 : 1;
    }
    NSNetwork::NSFileTransport::CFileDownloader download(wideUrl, operation == "existing");
    download.SetFilePath(wideFile);
    return download.DownloadSync() ? 0 : 1;
}
