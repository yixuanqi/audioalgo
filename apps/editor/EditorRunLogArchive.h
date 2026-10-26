#pragma once
#include "audio_editor_application/RunLogMailbox.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace audioalgo::editor {
struct RunLogArchiveReference {
    RunIdentity identity;
    std::filesystem::path file;
    std::uint64_t received_unix_ms = 0;
};
struct RunLogArchiveList {
    std::vector<RunLogArchiveReference> runs;
    std::string notice;
};
struct RunLogArchiveTail {
    std::deque<NodeLogEvent> events;
    std::string notice;
};

// Worker-only file operations. The view receives owned bounded values.
class RunLogArchiveReader {
    static std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
        auto value = std::filesystem::absolute(path).lexically_normal().wstring();
        if (!value.starts_with(L"\\\\?\\")) value = value.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + value.substr(2) : L"\\\\?\\" + value;
        return value;
#else
        return path;
#endif
    }
    static bool regular(const std::filesystem::path& path) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(io_path(path), ec);
        return !ec && std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status);
    }
    static NodeLogEvent parse(const std::string& line, const std::filesystem::path& file) {
        const auto row = juce::JSON::parse(juce::String::fromUTF8(line.data(), static_cast<int>(line.size())));
        if (!row.isObject()) throw std::runtime_error("Malformed archived log record");
        auto text = [&](const char* key) { if (!row[key].isString()) throw std::runtime_error("Invalid archived log text field");return row[key].toString().toStdString(); };
        auto count = [&](const char* key) { if ((!row[key].isInt() && !row[key].isInt64()) || static_cast<juce::int64>(row[key]) < 0) throw std::runtime_error("Invalid archived log counter");return static_cast<std::uint64_t>(static_cast<juce::int64>(row[key])); };
        NodeLogEvent event;
        event.identity={text("project_id"),count("session_epoch"),text("request_id"),count("revision")};
        event.node_id=text("node_id");event.type_id=text("type_id");event.package_id=text("package_id");event.content_sha256=text("content_sha256");
        event.version=text("version");event.source=text("source");event.level=text("level");event.text=text("text");event.raw_log_file=text("raw_log_file");
        if(row.hasProperty("text_encoding")) {
            if(text("text_encoding")!="utf8-base64")throw std::runtime_error("Unsupported archived log text encoding");
            const auto encoded=text("text_utf8_base64");juce::MemoryOutputStream decoded;
            if(!juce::Base64::convertFromBase64(decoded,juce::String(encoded)) ||
               juce::Base64::toBase64(decoded.getData(),decoded.getDataSize()).toStdString()!=encoded)
                throw std::runtime_error("Invalid archived log byte encoding");
            event.text.assign(static_cast<const char*>(decoded.getData()),decoded.getDataSize());
        }
        event.sequence=count("sequence");event.received_unix_ms=count("received_unix_ms");event.total_bytes=count("total_bytes");
        event.discarded_bytes=count("discarded_bytes");event.dropped_text_bytes=count("dropped_text_bytes");event.report_file=file.generic_string();
        return event;
    }
public:
    static RunLogArchiveList list(const std::filesystem::path& directory, const std::string& project,
                                  const std::function<bool()>& cancelled) {
        RunLogArchiveList result;
        if(cancelled())return result;
        std::error_code ec;std::filesystem::directory_iterator it(io_path(directory),ec),end;
        if(ec){if(ec!=std::errc::no_such_file_or_directory)result.notice="Could not read log history: "+ec.message();return result;}
        std::size_t skipped=0,matched=0;
        for(;it!=end&&!cancelled();it.increment(ec)) {
            if(ec){++skipped;break;}
            std::error_code status_error;const auto status=it->symlink_status(status_error);
            if(status_error||!std::filesystem::is_directory(status)||std::filesystem::is_symlink(status))continue;
            const auto file=directory/it->path().filename()/"events.jsonl";
            if(!regular(file))continue;
            try {
                std::ifstream input(io_path(file),std::ios::binary);std::string line;
                // Read only the first bounded record, never descend task/code trees.
                char c=0;while(input.get(c)&&c!='\n'&&line.size()<=1024*1024)line+=c;
                if(c!='\n'||line.size()>1024*1024)throw std::runtime_error("Incomplete log header");
                const auto event=parse(line,file);
                if(event.identity.project_id!=project)continue;
                ++matched;
                result.runs.push_back({event.identity,file,event.received_unix_ms});
                std::sort(result.runs.begin(),result.runs.end(),[](const auto& a,const auto& b){return a.received_unix_ms!=b.received_unix_ms?a.received_unix_ms>b.received_unix_ms:a.identity.request_id>b.identity.request_id;});
                if(result.runs.size()>128)result.runs.pop_back();
            }catch(const std::exception&){++skipped;}
        }
        if(ec)result.notice="Log history scan incomplete: "+ec.message();
        else if(skipped)result.notice=std::to_string(skipped)+" unreadable/incomplete log archives omitted";
        if(matched>128)result.notice+=(result.notice.empty()?"":"; ")+std::string("Showing the 128 most recent logged Runs");
        return result;
    }
    static RunLogArchiveTail load(const RunLogArchiveReference& reference, const std::function<bool()>& cancelled) {
        RunLogArchiveTail result;
        try {
            if(cancelled())return result;
            if(!regular(reference.file))throw std::runtime_error("Log archive is unavailable or is a link");
            std::ifstream input(io_path(reference.file),std::ios::binary|std::ios::ate);
            if(!input)throw std::runtime_error("Could not open log archive");
            const auto size=input.tellg();if(size<0)throw std::runtime_error("Could not measure log archive");
            constexpr std::streamoff suffix_limit=2*1024*1024;
            const auto start=std::max<std::streamoff>(0,static_cast<std::streamoff>(size)-suffix_limit);
            input.seekg(start);std::string bytes(static_cast<std::size_t>(static_cast<std::streamoff>(size)-start),'\0');
            input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));if(input.gcount()!=static_cast<std::streamsize>(bytes.size()))throw std::runtime_error("Log archive changed while reading");
            if(start){auto newline=bytes.find('\n');if(newline==std::string::npos)throw std::runtime_error("Archived record exceeds history read budget");bytes.erase(0,newline+1);result.notice="Showing archived log tail";}
            std::istringstream rows(bytes);std::string line;std::size_t retained=0;
            while(!cancelled()&&std::getline(rows,line)){
                if(rows.eof()){result.notice="Incomplete final log record omitted";break;}
                if(line.size()>1024*1024)throw std::runtime_error("Archived record exceeds history read budget");
                auto event=parse(line,reference.file);if(event.identity!=reference.identity)throw std::runtime_error("Archive Run identity changed");
                constexpr std::size_t limit=256*1024;
                const auto metadata=RunLogMailbox::weight(event)-event.text.size();if(metadata>=limit)throw std::runtime_error("Archived metadata exceeds history budget");
                event.dropped_text_bytes+=trim_log_text(event.text,limit-metadata);
                const auto weight=RunLogMailbox::weight(event);
                while(!result.events.empty()&&retained+weight>limit){retained-=RunLogMailbox::weight(result.events.front());result.events.pop_front();result.notice="Showing archived log tail";}
                retained+=weight;result.events.push_back(std::move(event));
            }
        }catch(const std::exception& error){result.events.clear();result.notice=error.what();}
        return result;
    }
};
} // namespace audioalgo::editor
