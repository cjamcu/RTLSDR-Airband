#pragma once
#include <string>
#include "rtl_airband.h"

void init_file_uploader();
void enqueue_upload(const std::string& path, const file_data& data);
void process_upload_queue();
void scan_pending_uploads();
