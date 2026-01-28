#pragma once
#include <string>

void debug_log(const std::string& file,
               const std::string& function,
               const std::string& message);

               
#define DEBUG(msg) \
    debug_log(__FILE__, __func__, msg)
