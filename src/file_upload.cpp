#include "file_upload.h"
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <cstdio>
#include <curl/curl.h>

struct upload_task {
    std::string path;
    file_data config;
    time_t next_try;
};

static std::queue<upload_task> upload_queue;
static std::mutex queue_mutex;
static std::atomic<bool> uploader_running;
static std::thread uploader_thread;

static void process_upload_queue();

void init_file_uploader() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    uploader_running = true;
    uploader_thread = std::thread([]() {
        while (uploader_running) {
            process_upload_queue();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    uploader_thread.detach();
}

static bool upload_file(const upload_task& task) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    curl_mime* form = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(form);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, task.path.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, task.config.upload_url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);

    CURLcode res = curl_easy_perform(curl);
    curl_mime_free(form);
    curl_easy_cleanup(curl);

    return res == CURLE_OK;
}

void enqueue_upload(const std::string& path, const file_data& data) {
    if (path.empty() || data.upload_url.empty())
        return;
    std::lock_guard<std::mutex> lock(queue_mutex);
    upload_queue.push({path, data, 0});
}

static void process_upload_queue() {
    std::queue<upload_task> work;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        std::swap(work, upload_queue);
    }
    while (!work.empty()) {
        upload_task task = work.front();
        work.pop();
        time_t now = time(NULL);
        if (task.next_try > now) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            upload_queue.push(task);
            continue;
        }
        if (upload_file(task)) {
            if (task.config.delete_after_upload) {
                unlink(task.path.c_str());
            } else {
                std::string renamed = task.path;
                size_t dot = renamed.find_last_of('.');
                if (dot != std::string::npos) {
                    renamed.insert(dot, "_uploaded");
                } else {
                    renamed += "_uploaded";
                }
                rename(task.path.c_str(), renamed.c_str());
            }
        } else {
            task.next_try = time(NULL) + task.config.upload_retry_interval;
            std::lock_guard<std::mutex> lock(queue_mutex);
            upload_queue.push(task);
        }
    }
}

static void scan_directory(const file_data& cfg, const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.')
            continue;
        std::string path = dir + "/" + ent->d_name;
        if (ent->d_type == DT_DIR && cfg.dated_subdirectories) {
            scan_directory(cfg, path);
        } else if (ent->d_type == DT_REG) {
            std::string filename = ent->d_name;
            size_t dot = filename.find_last_of('.');
            std::string stem = dot != std::string::npos ? filename.substr(0, dot) : filename;
            if (stem.size() >= sizeof("_uploaded") - 1 &&
                stem.substr(stem.size() - (sizeof("_uploaded") - 1)) == "_uploaded") {
                continue;
            }
            if (cfg.suffix.empty() ||
                (path.size() >= cfg.suffix.size() &&
                 path.substr(path.size() - cfg.suffix.size()) == cfg.suffix)) {
                enqueue_upload(path, cfg);
            }
        }
    }
    closedir(d);
}

void scan_pending_uploads() {
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* ch = &dev->channels[j];
            for (int k = 0; k < ch->output_count; k++) {
                output_t* out = &ch->outputs[k];
                if (out->type == O_FILE) {
                    file_data* fdata = (file_data*)out->data;
                    if (fdata && !fdata->upload_url.empty() && fdata->upload_pending_on_start) {
                        scan_directory(*fdata, fdata->basedir);
                    }
                }
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        if (!mixers[i].enabled)
            continue;
        channel_t* ch = &mixers[i].channel;
        for (int k = 0; k < ch->output_count; k++) {
            output_t* out = &ch->outputs[k];
            if (out->type == O_FILE) {
                file_data* fdata = (file_data*)out->data;
                if (fdata && !fdata->upload_url.empty() && fdata->upload_pending_on_start) {
                    scan_directory(*fdata, fdata->basedir);
                }
            }
        }
    }
}
