#pragma once
#include <string>
#include "Repository.h"

class HttpServer {
public:
    static void run(Repository* repo, int preferredPort = 8080, bool openBrowser = true, bool lan = false);
};
