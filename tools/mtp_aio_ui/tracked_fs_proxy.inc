// A thin wrapper used only by AIO MOD's normal microSD mount. It observes
// successful writes for the on-screen copy meter while delegating all file
// operations to Sphaira's proven native filesystem proxy.
struct FsTrackedProxy final : haze::FileSystemProxyImpl {
    struct File {
        haze::File inner{};
        std::string path;
        u64 total{};
        u64 copied{};
        u64 copy_id{};
        Result failure{};
        bool writing{};
    };

    explicit FsTrackedProxy(std::shared_ptr<haze::FileSystemProxyImpl> inner)
    : m_inner{std::move(inner)} {
    }

    const char* GetName() const override { return m_inner->GetName(); }
    const char* GetDisplayName() const override { return m_inner->GetDisplayName(); }
    bool IsReadOnly() override { return m_inner->IsReadOnly(); }

    Result GetTotalSpace(const char* path, s64* out) override { return m_inner->GetTotalSpace(path, out); }
    Result GetFreeSpace(const char* path, s64* out) override { return m_inner->GetFreeSpace(path, out); }
    Result GetEntryType(const char* path, haze::FileAttrType* out) override { return m_inner->GetEntryType(path, out); }
    Result GetEntryAttributes(const char* path, haze::FileAttr* out) override { return m_inner->GetEntryAttributes(path, out); }
    Result CreateFile(const char* path, s64 size) override { return m_inner->CreateFile(path, size); }
    Result DeleteFile(const char* path) override { return m_inner->DeleteFile(path); }
    Result RenameFile(const char* old_path, const char* new_path) override { return m_inner->RenameFile(old_path, new_path); }

    Result OpenFile(const char* path, haze::FileOpenMode mode, haze::File* out_file) override {
        auto file = new File();
        file->path = path ? path : "";
        file->writing = mode == haze::FileOpenMode_WRITE;
        const auto result = m_inner->OpenFile(path, mode, &file->inner);
        if (R_FAILED(result)) {
            delete file;
            return result;
        }
        out_file->impl = file;
        R_SUCCEED();
    }

    Result GetFileSize(haze::File* file, s64* out_size) override {
        auto tracked = static_cast<File*>(file->impl);
        return m_inner->GetFileSize(&tracked->inner, out_size);
    }

    Result SetFileSize(haze::File* file, s64 size) override {
        auto tracked = static_cast<File*>(file->impl);
        const auto result = m_inner->SetFileSize(&tracked->inner, size);
        if (R_SUCCEEDED(result) && tracked->writing && size >= 0) {
            // libhaze truncates an interrupted transfer to the bytes received
            // just before closing it. Preserve the original announced size so
            // CloseFile can still identify and report an incomplete copy.
            const auto new_size = static_cast<u64>(size);
            tracked->total = tracked->copy_id ? std::max(tracked->total, new_size) : new_size;
        }
        return result;
    }

    Result ReadFile(haze::File* file, s64 off, void* buf, u64 read_size, u64* out_bytes_read) override {
        auto tracked = static_cast<File*>(file->impl);
        return m_inner->ReadFile(&tracked->inner, off, buf, read_size, out_bytes_read);
    }

    Result WriteFile(haze::File* file, s64 off, const void* buf, u64 write_size) override {
        auto tracked = static_cast<File*>(file->impl);
        if (!tracked->copy_id) {
            tracked->copy_id = ui::activity_log::BeginCopy(tracked->path, tracked->total);
        }

        const auto result = m_inner->WriteFile(&tracked->inner, off, buf, write_size);
        if (R_FAILED(result)) {
            tracked->failure = result;
            return result;
        }

        const auto end = off >= 0 ? static_cast<u64>(off) + write_size : tracked->copied + write_size;
        tracked->copied = std::max(tracked->copied, end);
        ui::activity_log::UpdateCopy(tracked->copy_id, tracked->copied, tracked->total);
        return result;
    }

    void CloseFile(haze::File* file) override {
        auto tracked = static_cast<File*>(file->impl);
        if (!tracked) {
            return;
        }

        m_inner->CloseFile(&tracked->inner);
        if (tracked->copy_id) {
            const bool complete = R_SUCCEEDED(tracked->failure) &&
                (!tracked->total || tracked->copied >= tracked->total);
            ui::activity_log::FinishCopy(tracked->copy_id, complete, tracked->failure);
        }
        delete tracked;
        file->impl = nullptr;
    }

    Result CreateDirectory(const char* path) override { return m_inner->CreateDirectory(path); }
    Result DeleteDirectoryRecursively(const char* path) override { return m_inner->DeleteDirectoryRecursively(path); }
    Result RenameDirectory(const char* old_path, const char* new_path) override { return m_inner->RenameDirectory(old_path, new_path); }
    Result OpenDirectory(const char* path, haze::Dir* out_dir) override { return m_inner->OpenDirectory(path, out_dir); }
    Result ReadDirectory(haze::Dir* dir, s64* out_total_entries, size_t max_entries, haze::DirEntry* buf) override {
        return m_inner->ReadDirectory(dir, out_total_entries, max_entries, buf);
    }
    Result GetDirectoryEntryCount(haze::Dir* dir, s64* out_count) override {
        return m_inner->GetDirectoryEntryCount(dir, out_count);
    }
    void CloseDirectory(haze::Dir* dir) override { m_inner->CloseDirectory(dir); }

private:
    std::shared_ptr<haze::FileSystemProxyImpl> m_inner;
};
