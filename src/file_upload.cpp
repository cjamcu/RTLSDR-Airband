#include "file_upload.h"
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <set>
#include <thread>

struct upload_task {
    std::string path;
    file_data config;
    time_t next_try;
};

struct task_compare {
    bool operator()(const upload_task& a, const upload_task& b) const { return a.next_try > b.next_try; }
};

static std::priority_queue<upload_task, std::vector<upload_task>, task_compare> upload_queue;
static std::set<std::string> queued_files;
static std::mutex queue_mutex;
static std::condition_variable queue_cv;
static std::atomic<bool> uploader_running;
static std::thread uploader_thread;

static bool upload_file(const upload_task& task) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        log(LOG_ERR, "curl_easy_init() failed\n");
        return false;
    }

    curl_mime* form = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(form);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, task.path.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, task.config.upload_url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }
    if (res != CURLE_OK) {
        log(LOG_ERR, "Upload of %s failed: %s\n", task.path.c_str(), curl_easy_strerror(res));
    } else if (http_code < 200 || http_code >= 300) {
        log(LOG_ERR, "Upload of %s returned HTTP %ld\n", task.path.c_str(), http_code);
    }

    curl_mime_free(form);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && http_code >= 200 && http_code < 300;
}

void enqueue_upload(const std::string& path, const file_data& data) {
    if (path.empty() || data.upload_url.empty())
        return;
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (queued_files.insert(path).second) {
        upload_queue.push({path, data, 0});
        queue_cv.notify_all();
    }
}
void init_file_uploader() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    uploader_running = true;
    uploader_thread = std::thread([]() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        while (uploader_running) {
            if (upload_queue.empty()) {
                queue_cv.wait(lock, [] { return !uploader_running || !upload_queue.empty(); });
                if (!uploader_running)
                    break;
            } else {
                time_t now = time(NULL);
                time_t next = upload_queue.top().next_try;
                if (next > now) {
                    queue_cv.wait_until(lock, std::chrono::system_clock::from_time_t(next));
                    continue;
                }

                upload_task task = upload_queue.top();
                upload_queue.pop();
                queued_files.erase(task.path);
                lock.unlock();

                bool ok = upload_file(task);
                if (ok) {
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
                    std::lock_guard<std::mutex> relock(queue_mutex);
                    queued_files.insert(task.path);
                    upload_queue.push(task);
                    queue_cv.notify_all();
                }
                lock.lock();
            }
        }
    });
}

void shutdown_file_uploader() {
    uploader_running = false;
    queue_cv.notify_all();
    if (uploader_thread.joinable())
        uploader_thread.join();
    curl_global_cleanup();
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
            if (stem.size() >= sizeof("_uploaded") - 1 && stem.substr(stem.size() - (sizeof("_uploaded") - 1)) == "_uploaded") {
                continue;
            }
            if (cfg.suffix.empty() || (path.size() >= cfg.suffix.size() && path.substr(path.size() - cfg.suffix.size()) == cfg.suffix)) {
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

