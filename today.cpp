// today.cpp — C++20 port of today.py (1:1 behaviour, names kept from the Python).
// Env: ACCESS_TOKEN (GitHub token), USER_NAME (GitHub login).
//
// # port: libcurl replaces `requests` and a hand-written JSON parser replaces
// `r.json()`. STL has neither HTTP nor JSON, and the spec allowed libcurl here.
// # port: Python's `datetime.date.today()` is the LOCAL date; std::localtime is
// used to match that instead of UTC.

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static const std::string TOKEN = std::getenv("ACCESS_TOKEN") ? std::getenv("ACCESS_TOKEN") : "";
static const std::string USER = std::getenv("USER_NAME") ? std::getenv("USER_NAME") : "";
static const std::string API = "https://api.github.com/graphql";
static const std::vector<std::string> SVGS = {"dark_mode.svg", "light_mode.svg"};

// ---------------------------------------------------------------- JSON ----
// Minimal recursive-descent parser covering exactly what the two GraphQL
// queries return: objects, arrays, strings, numbers, bools, null.

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    long long number = 0;
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : object)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class JsonParser {
  public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        if (pos_ != text_.size()) fail("trailing data after top-level value");
        return v;
    }

  private:
    const std::string& text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error("json: " + why + " at offset " + std::to_string(pos_));
    }

    void skip_ws() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    char peek() {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_];
    }

    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    JsonValue parse_value() {
        switch (peek()) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': {
            JsonValue v;
            v.type = JsonValue::Type::String;
            v.str = parse_string();
            return v;
        }
        case 't': case 'f': return parse_bool();
        case 'n': return parse_null();
        default: return parse_number();
        }
    }

    JsonValue parse_object() {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        expect('{');
        skip_ws();
        if (peek() == '}') { ++pos_; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            v.object.emplace_back(std::move(key), parse_value());
            skip_ws();
            char c = peek();
            ++pos_;
            if (c == '}') return v;
            if (c != ',') fail("expected ',' or '}' in object");
        }
    }

    JsonValue parse_array() {
        JsonValue v;
        v.type = JsonValue::Type::Array;
        expect('[');
        skip_ws();
        if (peek() == ']') { ++pos_; return v; }
        while (true) {
            skip_ws();
            v.array.push_back(parse_value());
            skip_ws();
            char c = peek();
            ++pos_;
            if (c == ']') return v;
            if (c != ',') fail("expected ',' or ']' in array");
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) fail("unterminated string");
            char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) fail("unterminated escape");
            char e = text_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': out += parse_unicode_escape(); break;
            default: fail("invalid escape");
            }
        }
    }

    unsigned parse_hex4() {
        if (pos_ + 4 > text_.size()) fail("truncated \\u escape");
        unsigned cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid hex digit in \\u escape");
        }
        return cp;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string parse_unicode_escape() {
        unsigned cp = parse_hex4();
        std::string out;
        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
            text_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned low = parse_hex4();
            if (low >= 0xDC00 && low <= 0xDFFF)
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            else {
                append_utf8(out, cp);
                cp = low;
            }
        }
        append_utf8(out, cp);
        return out;
    }

    JsonValue parse_bool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (text_.compare(pos_, 4, "true") == 0) { v.boolean = true; pos_ += 4; }
        else if (text_.compare(pos_, 5, "false") == 0) { v.boolean = false; pos_ += 5; }
        else fail("invalid literal");
        return v;
    }

    JsonValue parse_null() {
        if (text_.compare(pos_, 4, "null") != 0) fail("invalid literal");
        pos_ += 4;
        return JsonValue{};
    }

    JsonValue parse_number() {
        std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '.' ||
                text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-')) {
            any = true;
            ++pos_;
        }
        if (!any) fail("invalid number");
        // # port: Python's json returns float for 2.9 and the original would then
        // keep it as a float. Both GraphQL queries only select integer fields
        // (totalCount, stargazerCount, totalContributions), so a non-integer here
        // means the response shape changed. Reject instead of truncating.
        for (std::size_t i = start; i < pos_; ++i)
            if (text_[i] == '.' || text_[i] == 'e' || text_[i] == 'E')
                fail("non-integer number not supported by this port");
        JsonValue v;
        v.type = JsonValue::Type::Number;
        v.number = std::stoll(text_.substr(start, pos_ - start));
        return v;
    }
};

// Python's json module, used to build request bodies.
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFF);
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

static std::string json_string(const std::string& s) { return "\"" + json_escape(s) + "\""; }

// --------------------------------------------------------------- HTTP ----

static std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string http_post(const std::string& url, const std::string& body) {
    std::string response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");

    std::string auth = "Authorization: bearer " + TOKEN;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "User-Agent: today.cpp");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
    if (status < 200 || status >= 300)
        throw std::runtime_error("HTTP " + std::to_string(status) + ": " + response);
    return response;
}

// ------------------------------------------------------------ business ----

static JsonValue gql(const std::string& query, const std::string& variables) {
    std::string body = "{\"query\":" + json_string(query) + ",\"variables\":" + variables + "}";
    JsonValue data = JsonParser(http_post(API, body)).parse();
    if (data.find("errors") != nullptr)
        throw std::runtime_error("GraphQL errors in response: " + data.find("errors")->str);
    const JsonValue* d = data.find("data");
    if (d == nullptr) throw std::runtime_error("no data field in GraphQL response");
    return *d;
}

struct Stats {
    long long repos = 0;
    long long stars = 0;
    long long followers = 0;
    std::string created;
};

static Stats repo_and_star_stats() {
    static const std::string q =
        "query($login:String!,$cursor:String){user(login:$login){\n"
        "      createdAt followers{totalCount}\n"
        "      repositories(first:100,after:$cursor,ownerAffiliations:OWNER){\n"
        "        totalCount nodes{stargazerCount} pageInfo{hasNextPage endCursor}}}}";

    std::string cursor_json = "null";
    long long stars = 0;
    while (true) {
        std::string variables =
            std::string("{\"login\":") + json_string(USER) + ",\"cursor\":" + cursor_json + "}";
        const JsonValue data = gql(q, variables);
        const JsonValue* u = data.find("user");
        if (u == nullptr) throw std::runtime_error("no user in GraphQL response");
        const JsonValue* repos = u->find("repositories");
        if (repos == nullptr) throw std::runtime_error("no repositories in GraphQL response");

        const JsonValue* nodes = repos->find("nodes");
        if (nodes != nullptr)
            for (const auto& n : nodes->array) {
                const JsonValue* s = n.find("stargazerCount");
                if (s != nullptr) stars += s->number;
            }

        const JsonValue* page_info = repos->find("pageInfo");
        const bool has_next =
            page_info != nullptr && page_info->find("hasNextPage") != nullptr &&
            page_info->find("hasNextPage")->boolean;
        if (!has_next) {
            Stats out;
            out.repos = repos->find("totalCount")->number;
            out.stars = stars;
            const JsonValue* followers = u->find("followers");
            out.followers = followers != nullptr && followers->find("totalCount") != nullptr
                                ? followers->find("totalCount")->number
                                : 0;
            const JsonValue* created = u->find("createdAt");
            out.created = created != nullptr ? created->str : std::string();
            return out;
        }
        const JsonValue* end = page_info->find("endCursor");
        if (end == nullptr) throw std::runtime_error("pageInfo.endCursor missing while hasNextPage");
        cursor_json = json_string(end->str);
    }
}

static long long total_contributions(const std::string& created_at) {
    static const std::string q =
        "query($login:String!,$from:DateTime!,$to:DateTime!){user(login:$login){\n"
        "      contributionsCollection(from:$from,to:$to){contributionCalendar{totalContributions}}}}";

    const std::time_t now = std::time(nullptr);
    const int this_year = std::localtime(&now)->tm_year + 1900;
    const int from_year = std::stoi(created_at.substr(0, 4));

    long long total = 0;
    for (int year = from_year; year <= this_year; ++year) {
        const std::string from = std::to_string(year) + "-01-01T00:00:00Z";
        const std::string to = std::to_string(year) + "-12-31T23:59:59Z";
        std::string variables = "{\"login\":" + json_string(USER) + ",\"from\":" + json_string(from) +
                                ",\"to\":" + json_string(to) + "}";
        const JsonValue data = gql(q, variables);
        const JsonValue* d = data.find("user");
        if (d == nullptr) throw std::runtime_error("no user in contributions response");
        const JsonValue* cc = d->find("contributionsCollection");
        if (cc == nullptr) throw std::runtime_error("no contributionsCollection in response");
        const JsonValue* cal = cc->find("contributionCalendar");
        if (cal == nullptr) throw std::runtime_error("no contributionCalendar in response");
        total += cal->find("totalContributions")->number;
    }
    return total;
}

// Python's f'{value:,}' — std::format needs GCC 13+, this keeps GCC 12 working.
static std::string thousands(long long v) {
    const bool negative = v < 0;
    const std::string digits = std::to_string(negative ? -v : v);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return negative ? "-" + out : out;
}

// Replace the text between `anchor` and the next '<'. Mirrors re.sub on `[^<]*`.
static std::string replace_between(const std::string& svg, const std::string& anchor,
                                   const std::string& replacement) {
    const std::size_t at = svg.find(anchor);
    if (at == std::string::npos)
        throw std::runtime_error("set_value: anchor not found: " + anchor);
    const std::size_t start = at + anchor.size();
    const std::size_t end = svg.find('<', start);
    if (end == std::string::npos)
        throw std::runtime_error("set_value: no closing '<' after: " + anchor);
    return svg.substr(0, start) + replacement + svg.substr(end);
}

static void set_value(std::string& svg, const std::string& elem_id, const std::string& value) {
    const std::string id_anchor = "id=\"" + elem_id + "\" data-len=\"";
    const std::size_t at = svg.find(id_anchor);
    if (at == std::string::npos)
        throw std::runtime_error("set_value: no data-len for " + elem_id);
    const std::size_t digits_at = at + id_anchor.size();
    const std::size_t quote = svg.find('"', digits_at);
    if (quote == std::string::npos)
        throw std::runtime_error("set_value: unterminated data-len for " + elem_id);
    const long avail = std::stol(svg.substr(digits_at, quote - digits_at));

    const long dots_n = std::max(1L, avail - static_cast<long>(value.size()) - 2);
    const std::string dots = " " + std::string(static_cast<std::size_t>(dots_n), '.') + " ";

    // Build both anchors before mutating: the dots replacement changes svg's length.
    const std::string dots_anchor = "id=\"" + elem_id + "_dots\">";
    const std::string value_anchor = id_anchor + svg.substr(digits_at, quote - digits_at) + "\">";

    svg = replace_between(svg, dots_anchor, dots);
    svg = replace_between(svg, value_anchor, value);
}

static void set_value(std::string& svg, const std::string& elem_id, long long value) {
    set_value(svg, elem_id, thousands(value));
}

static void update_svgs(const std::vector<std::pair<std::string, long long>>& stats) {
    for (const auto& path : SVGS) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + path);
        std::ostringstream buf;
        buf << in.rdbuf();
        std::string svg = buf.str();
        for (const auto& kv : stats) set_value(svg, kv.first, kv.second);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + path);
        out.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    }
}

int main() {
    try {
        Stats s = repo_and_star_stats();
        long long commits = total_contributions(s.created);
        update_svgs({{"repo_data", s.repos},
                      {"star_data", s.stars},
                      {"follower_data", s.followers},
                      {"commit_data", commits}});
        std::cout << "updated: " << s.repos << " " << s.stars << " " << s.followers << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}