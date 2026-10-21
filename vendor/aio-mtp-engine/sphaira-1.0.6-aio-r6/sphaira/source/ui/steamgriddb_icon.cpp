#include "ui/steamgriddb_icon.hpp"

#include "app.hpp"
#include "download.hpp"
#include "i18n.hpp"
#include "image.hpp"
#include "option.hpp"
#include "swkbd.hpp"
#include "ui/list.hpp"
#include "ui/nvg_util.hpp"
#include "ui/progress_box.hpp"
#include "ui/widget.hpp"

#include <yyjson.h>

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace sphaira::ui::steamgriddb {
namespace {

constexpr size_t ICON_BATCH_SIZE = 4;
constexpr size_t MAX_IMAGE_DOWNLOAD_SIZE = 8 * 1024 * 1024;

option::OptionString g_api_key{"steamgriddb", "api_key", ""};

enum class SearchError {
    None,
    InvalidKey,
    Network,
    NotFound,
    InvalidResponse,
    Cancelled,
};

struct SearchState {
    SearchError error{SearchError::None};
    std::string match_name;
    std::vector<std::string> urls;
    std::vector<std::vector<u8>> icons;
    size_t next_url_index{};
};

auto DownloadIconBatch(ProgressBox* pbox, SearchState& state) -> Result;

class IconGrid final : public Widget {
public:
    IconGrid(std::string title, std::shared_ptr<SearchState> state, IconCallback callback)
    : m_title{std::move(title)}
    , m_state{std::move(state)}
    , m_callback{std::move(callback)} {
        SetActions(
            std::make_pair(Button::A, Action{"Select"_i18n, [this](){ Activate(); }}),
            std::make_pair(Button::B, Action{"Back"_i18n, [this](){
                SetPop();
            }})
        );

        const Vec4 list_pos{55.f, 115.f, 1170.f, 510.f};
        const Vec4 item_pos{85.f, 125.f, 200.f, 200.f};
        m_list = std::make_unique<List>(5, 10, list_pos, item_pos, Vec2{20.f, 20.f});
        m_list->SetScrollBarPos(1240.f, 125.f, 490.f);

        SyncImages();
    }

    ~IconGrid() override {
        auto vg = App::GetVg();
        for (auto image : m_images) {
            if (image > 0) {
                nvgDeleteImage(vg, image);
            }
        }
    }

    void Update(Controller* controller, TouchInfo* touch) override {
        Widget::Update(controller, touch);
        m_list->OnUpdate(controller, touch, m_index, GetItemCount(), [this](bool touched, s64 index){
            if (touched && m_index == index) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_index = index;
            }
        });
    }

    void Draw(NVGcontext* vg, Theme* theme) override {
        DrawElement(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, ThemeEntryID_BACKGROUND);
        gfx::drawText(vg, 70.f, 55.f, 28.f, theme->GetColour(ThemeEntryID_TEXT), "Choose an Icon"_i18n.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        const auto match_text = m_title == m_state->match_name ? m_state->match_name : m_title + "  →  " + m_state->match_name;
        gfx::drawText(vg, 1210.f, 55.f, 18.f, theme->GetColour(ThemeEntryID_TEXT_INFO), match_text.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        gfx::drawRect(vg, 30.f, 86.f, 1220.f, 1.f, theme->GetColour(ThemeEntryID_LINE));
        gfx::drawRect(vg, 30.f, 646.f, 1220.f, 1.f, theme->GetColour(ThemeEntryID_LINE));

        m_list->Draw(vg, theme, GetItemCount(), [this](auto* vg, auto* theme, const Vec4& pos, s64 index){
            if (m_index == index) {
                gfx::drawRectOutline(vg, theme, 4.f, pos);
            } else {
                DrawElement(pos, ThemeEntryID_GRID);
            }

            if (index >= m_state->icons.size()) {
                const auto colour = theme->GetColour(m_index == index ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT);
                gfx::drawText(vg, pos.x + pos.w / 2.f, pos.y + 72.f, 52.f, colour, "+", NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                gfx::drawText(vg, pos.x + pos.w / 2.f, pos.y + 132.f, 19.f, colour, "Load 4 More"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                const auto remaining = m_state->urls.size() - m_state->next_url_index;
                const auto remaining_text = std::to_string(remaining) + " " + "remaining"_i18n;
                gfx::drawText(vg, pos.x + pos.w / 2.f, pos.y + 165.f, 15.f, theme->GetColour(ThemeEntryID_TEXT_INFO), remaining_text.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                return;
            }

            const Vec4 image_pos{pos.x + 8.f, pos.y + 8.f, pos.w - 16.f, pos.h - 16.f};
            const auto image = m_images[index] > 0 ? m_images[index] : App::GetDefaultImage();
            gfx::drawImage(vg, image_pos, image, 5.f);
        });

        Widget::Draw(vg, theme);
    }

private:
    auto HasMore() const -> bool {
        return m_state->next_url_index < m_state->urls.size();
    }

    auto GetItemCount() const -> size_t {
        return m_state->icons.size() + (HasMore() ? 1 : 0);
    }

    void SyncImages() {
        auto vg = App::GetVg();
        m_images.reserve(m_state->icons.size());
        while (m_images.size() < m_state->icons.size()) {
            const auto& icon = m_state->icons[m_images.size()];
            m_images.emplace_back(nvgCreateImageMem(vg, 0, icon.data(), icon.size()));
        }
    }

    void Activate() {
        if (m_index >= 0 && m_index < m_state->icons.size()) {
            if (m_callback) {
                m_callback(std::move(m_state->icons[m_index]));
            }
            SetPop();
            return;
        }

        if (!HasMore()) {
            return;
        }

        const auto previous_count = m_state->icons.size();
        App::Push<ProgressBox>(
            0, "Loading more SteamGridDB icons"_i18n, m_state->match_name,
            [state = m_state](auto pbox) -> Result {
                return DownloadIconBatch(pbox, *state);
            },
            [this, previous_count](Result) {
                SyncImages();
                if (m_state->icons.size() == previous_count) {
                    App::Notify("No more SteamGridDB icons could be downloaded"_i18n);
                }
            }
        );
    }

    const std::string m_title;
    const std::shared_ptr<SearchState> m_state;
    IconCallback m_callback;
    std::vector<int> m_images;
    std::unique_ptr<List> m_list;
    s64 m_index{};
};

auto GetResponseError(const curl::ApiResult& result) -> SearchError {
    if (result.code == 401 || result.code == 403) {
        return SearchError::InvalidKey;
    }
    if (!result.success) {
        return SearchError::Network;
    }
    if (result.code < 200 || result.code >= 300) {
        return SearchError::NotFound;
    }
    return SearchError::None;
}

auto ParseFirstGame(const std::vector<u8>& data, s64& id, std::string& name) -> bool {
    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    if (!doc) {
        return false;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    auto games = root ? yyjson_obj_get(root, "data") : nullptr;
    auto game = games && yyjson_is_arr(games) ? yyjson_arr_get_first(games) : nullptr;
    if (!game || !yyjson_is_obj(game)) {
        return false;
    }

    auto id_value = yyjson_obj_get(game, "id");
    auto name_value = yyjson_obj_get(game, "name");
    if (!yyjson_is_int(id_value) || !yyjson_is_str(name_value)) {
        return false;
    }

    id = yyjson_get_sint(id_value);
    name.assign(yyjson_get_str(name_value), yyjson_get_len(name_value));
    return id > 0 && !name.empty();
}

void AppendUrls(const std::vector<u8>& data, std::vector<std::string>& urls, std::unordered_set<std::string>& seen) {
    auto doc = yyjson_read(reinterpret_cast<const char*>(data.data()), data.size(), YYJSON_READ_NOFLAG);
    if (!doc) {
        return;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    auto entries = root ? yyjson_obj_get(root, "data") : nullptr;
    if (!entries || !yyjson_is_arr(entries)) {
        return;
    }

    size_t index, count;
    yyjson_val* entry;
    yyjson_arr_foreach(entries, index, count, entry) {
        auto value = yyjson_is_obj(entry) ? yyjson_obj_get(entry, "url") : nullptr;
        if (!yyjson_is_str(value)) {
            continue;
        }

        std::string url{yyjson_get_str(value), yyjson_get_len(value)};
        if (!url.empty() && seen.emplace(url).second) {
            urls.emplace_back(std::move(url));
        }
    }
}

auto ConvertToForwarderIcon(std::span<const u8> compressed) -> std::vector<u8> {
    if (compressed.empty() || compressed.size() > MAX_IMAGE_DOWNLOAD_SIZE) {
        return {};
    }

    return ImageNormalizeIcon(compressed);
}

auto DownloadIconBatch(ProgressBox* pbox, SearchState& state) -> Result {
    const auto batch_end = std::min(state.next_url_index + ICON_BATCH_SIZE, state.urls.size());
    const auto batch_size = batch_end - state.next_url_index;

    for (size_t batch_index = 0; state.next_url_index < batch_end; ++batch_index) {
        if (pbox->ShouldExit()) {
            R_SUCCEED();
        }

        const auto url_index = state.next_url_index++;
        pbox->NewTransfer("Downloading icon"_i18n).UpdateTransfer(batch_index, batch_size);
        const auto result = curl::Api().ToMemory(
            curl::Url{state.urls[url_index]},
            curl::StopToken{pbox->GetToken()},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );
        if (!result.success || result.code < 200 || result.code >= 300) {
            continue;
        }

        auto icon = ConvertToForwarderIcon(result.data);
        if (!icon.empty()) {
            pbox->SetImageDataConst(icon);
            state.icons.emplace_back(std::move(icon));
        }
    }

    R_SUCCEED();
}

auto FetchIcons(ProgressBox* pbox, const std::string& api_key, const std::string& title, SearchState& state) -> Result {
    pbox->NewTransfer("Searching for matching titles"_i18n);

    const auto search_url = "https://www.steamgriddb.com/api/v2/search/autocomplete/" + curl::EscapeString(title);
    const auto search = curl::Api().ToMemory(
        curl::Url{search_url},
        curl::Bearer{api_key},
        curl::StopToken{pbox->GetToken()},
        curl::OnProgress{pbox->OnDownloadProgressCallback()}
    );
    state.error = GetResponseError(search);
    if (state.error != SearchError::None) {
        R_SUCCEED();
    }

    s64 game_id{};
    if (!ParseFirstGame(search.data, game_id, state.match_name)) {
        state.error = SearchError::NotFound;
        R_SUCCEED();
    }

    pbox->SetTitle(state.match_name);
    pbox->NewTransfer("Loading available icons"_i18n);

    const std::string grids_url = "https://www.steamgriddb.com/api/v2/grids/game/" + std::to_string(game_id)
        + "?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/jpeg";
    const std::string icons_url = "https://www.steamgriddb.com/api/v2/icons/game/" + std::to_string(game_id)
        + "?types=static&mimes=image/png,image/jpeg";

    std::unordered_set<std::string> seen;
    for (const auto& url : {grids_url, icons_url}) {
        const auto result = curl::Api().ToMemory(
            curl::Url{url},
            curl::Bearer{api_key},
            curl::StopToken{pbox->GetToken()},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );
        const auto error = GetResponseError(result);
        if (error == SearchError::InvalidKey) {
            state.error = error;
            R_SUCCEED();
        }
        if (error == SearchError::None) {
            AppendUrls(result.data, state.urls, seen);
        }
    }

    if (state.urls.empty()) {
        state.error = SearchError::NotFound;
        R_SUCCEED();
    }

    state.icons.reserve(std::min(state.urls.size(), ICON_BATCH_SIZE));
    R_TRY(DownloadIconBatch(pbox, state));

    if (state.icons.empty()) {
        state.error = SearchError::NotFound;
    }
    R_SUCCEED();
}

void ShowSearchError(SearchError error) {
    switch (error) {
        case SearchError::InvalidKey:
            g_api_key.Set("");
            App::Notify("The SteamGridDB API key was rejected; try again"_i18n);
            break;
        case SearchError::Network:
            App::Notify("Could not connect to SteamGridDB"_i18n);
            break;
        case SearchError::NotFound:
            App::Notify("No matching SteamGridDB icons were found"_i18n);
            break;
        case SearchError::InvalidResponse:
            App::Notify("SteamGridDB returned an invalid response"_i18n);
            break;
        case SearchError::Cancelled:
        case SearchError::None:
            break;
    }
}

} // namespace

auto NormalizeIcon(std::span<const u8> icon) -> std::vector<u8> {
    return ConvertToForwarderIcon(icon);
}

void ShowIconPicker(const std::string& title, const IconCallback& callback) {
    if (title.empty()) {
        App::Notify("Enter an App Title before searching"_i18n);
        return;
    }

    auto api_key = g_api_key.Get();
    if (api_key.empty()) {
        if (R_FAILED(swkbd::ShowText(
            api_key,
            "SteamGridDB API Key"_i18n.c_str(),
            "Enter your API key from steamgriddb.com"_i18n.c_str(),
            nullptr, 1, 256
        )) || api_key.empty()) {
            App::Notify("A SteamGridDB API key is required"_i18n);
            return;
        }
        g_api_key.Set(api_key);
    }

    auto state = std::make_shared<SearchState>();
    App::Push<ProgressBox>(
        0, "Searching SteamGridDB"_i18n, title,
        [state, api_key, title](auto pbox) -> Result {
            return FetchIcons(pbox, api_key, title, *state);
        },
        [state, title, callback](Result) {
            if (state->error != SearchError::None) {
                ShowSearchError(state->error);
                return;
            }
            App::Push<IconGrid>(title, state, callback);
        }
    );
}

} // namespace sphaira::ui::steamgriddb
