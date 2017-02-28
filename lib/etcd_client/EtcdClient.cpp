// Copyright 2016 Helvi Hartmann
// Copyright 2017 Jan de Cuveland <cmail@cuveland.de>

#include "EtcdClient.hpp"

EtcdClient::EtcdClient(std::string url_) : m_url(url_) {}

size_t write_callback(char* buf, size_t size, size_t nmemb, void* userdata)
{
    // callback must have this declaration
    std::string* data = reinterpret_cast<std::string*>(userdata);
    data->append(buf, size * nmemb);
    return size * nmemb;
}

std::pair<enum Flags, value_t> EtcdClient::get_req(std::string key, bool wait)
{
    std::string data;
    CURL* hnd = curl_easy_init();
    curl_easy_setopt(hnd, CURLOPT_URL, (m_url + key).c_str());
    if (wait) {
        L_(info) << "waiting for " << m_url << key;
        curl_easy_setopt(hnd, CURLOPT_TIMEOUT_MS, 8000L);
    }
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&data));
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, &write_callback);

    CURLcode ret = curl_easy_perform(hnd);
    std::pair<enum Flags, value_t> returnvalue(errorneous, {"", 1});
    if (ret == CURLE_OK)
        returnvalue = parse_value(data);
    else {
        L_(error) << curl_easy_strerror(ret) << std::endl;
        if (ret == 28)
            returnvalue.first = timeout;
    }

    curl_easy_cleanup(hnd);

    return returnvalue;
}

int EtcdClient::set_value(std::string key, std::string value)
{
    L_(info) << "Publishing " << value << " to " << m_url << key;
    CURL* hnd = curl_easy_init();
    std::string data;
    curl_easy_setopt(hnd, CURLOPT_URL, (m_url + key).c_str());
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDS, value.c_str());
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDSIZE_LARGE, strlen(value.c_str()));
    curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&data));
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, &write_callback);

    CURLcode ret = curl_easy_perform(hnd);
    if (ret != CURLE_OK)
        L_(error) << curl_easy_strerror(ret) << std::endl;
    curl_easy_cleanup(hnd);

    // if 0 ok, else error
    return ret;
}

enum Flags EtcdClient::wait_value(std::string key, int requiredtag)
{
    Flags wait_flag = empty;

    while (wait_flag == empty) {
        std::pair<enum Flags, value_t> returnvalue = get_req(
            key + "?wait=true&waitIndex=" + std::to_string(requiredtag), true);
        wait_flag = returnvalue.first;
        if (returnvalue.second.value == "on")
            wait_flag = ok;
    }
    return wait_flag;
}

std::pair<enum Flags, value_t> EtcdClient::parse_value(std::string data)
{
    Json::Value message;
    Json::Reader reader;
    reader.parse(data, message);
    // if parse_value is called curl request returned ok.
    // message contains error if key did not exist
    std::pair<enum Flags, value_t> returnvalue(ok, {"", 1});

    if (message.empty()) {
        L_(warning) << "EtcdClient: returned value from key-value store "
                       "was empty, waiting for updates";
        returnvalue.first = empty;
        return returnvalue;
    }

    if (message.isMember("errorCode")) {
        L_(error) << message["message"].asString() << std::endl;
        returnvalue.first = notexist;
    } else {
        returnvalue.second.value = message["node"]["value"].asString();
        returnvalue.second.tag = message["node"]["modifiedIndex"].asInt();
    }

    return returnvalue;
}

enum Flags EtcdClient::check_process(std::string input_shm)
{
    std::pair<enum Flags, value_t> returnvalue(notexist, {"", 1});
    while (returnvalue.first == notexist) {
        // I can not issue a wait on a key that does not exist
        returnvalue = get_req("/" + input_shm + "/uptodate", false);
    }
    if (returnvalue.first != errorneous) {
        if (returnvalue.second.value == "on")
            return ok;
        else {
            L_(info) << "no shm set in key-value store...waiting";
            returnvalue.first = wait_value("/" + input_shm + "/uptodate",
                                           returnvalue.second.tag + 1);
        }
    }

    return returnvalue.first;
}
