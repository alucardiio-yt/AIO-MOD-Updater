#include "hb_store.hpp"

#include <borealis.hpp>
#include <switch.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <array>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <json.hpp>
#include <mutex>
#include <thread>
#include "catalog_refresh_dialog.hpp"
#include "confirm_page.hpp"
#include "download.hpp"
#include "worker_page.hpp"

using namespace brls::i18n::literals;

namespace hb_store {
namespace {
using Direction = brls::FocusDirection;

// One worker per open view. A new page replaces the pending requests.
// The worker never touches a view or the application's download progress.
class Pictures {
    const bool banner;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> queue;
    std::map<std::string,std::string> completed;
    std::set<std::string> wanted, attempted;
    std::thread worker;
    void run()
    {
        for (;;) {
            std::string name;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock,[this]{ return stop || !queue.empty(); });
                if (stop) return;
                name = std::move(queue.front());
                queue.pop_front();
                attempted.insert(name);
            }
            try {
                auto path = artwork(name,banner,stop);
                if (!path.empty()) {
                    std::lock_guard lock(mutex);
                    if (wanted.contains(name)) completed[name] = std::move(path);
                }
            } catch (const std::exception&) { /* Artwork is optional. */ }
        }
    }
public:
    explicit Pictures(bool banner) : banner(banner)
    {
        try { worker = std::thread([this]{ run(); }); } catch (const std::system_error&) {}
    }
    ~Pictures()
    {
        { std::lock_guard lock(mutex); stop = true; }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
    void request(std::deque<std::string> names)
    {
        {
            std::lock_guard lock(mutex);
            wanted = std::set<std::string>(names.begin(), names.end());
            std::erase_if(attempted, [this](const auto& key){ return !wanted.contains(key); });
            std::erase_if(completed, [this](const auto& entry){ return !wanted.contains(entry.first); });
            std::erase_if(names, [this](const std::string& name){ return attempted.contains(name); });
            queue.swap(names);
        }
        changed.notify_one();
    }
    std::string path(const std::string& name)
    {
        std::lock_guard lock(mutex);
        auto it = completed.find(name);
        return it == completed.end() ? std::string{} : it->second;
    }
};

void installPrompt(const Package& p)
{
    auto* flow = new brls::StagedAppletFrame();
    flow->setTitle(p.title);
    const auto prompt = fmt::format("menus/ports/confirm_zip"_i18n,p.title) +
        download::spaceSummaryFromSize(p.downloadKiB * 1024, p.downloadKiB != 0, true);
    flow->addStage(new ConfirmPage(flow,prompt));
    flow->addStage(new WorkerPage(flow,p.title,[p]{ install(p); }));
    flow->addStage(new ConfirmPage_Done(flow,"menus/apps/done"_i18n));
    brls::Application::pushView(flow);
}


std::string tr(const char* key)
{
    return brls::i18n::internal::getRawStr(std::string("menus/hb_store/") + key);
}

// Preserve the service's category ID for filtering; translate only its label.
std::string categoryLabel(const std::string& id)
{
    if (id.empty() || id == "n/a") return "menus/ports/details_unknown"_i18n;
    static constexpr std::array<std::string_view,9> known = {
        "advanced","concept","demo","emu","game","legacy","misc","theme","tool"
    };
    if (std::find(known.begin(),known.end(),id) == known.end()) return id;
    return brls::i18n::internal::getRawStr("menus/hb_store/categories/" + id);
}

void rect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor color)
{
    nvgBeginPath(vg);
    nvgRect(vg,x,y,w,h);
    nvgFillColor(vg,color);
    nvgFill(vg);
}

void text(NVGcontext* vg,float x,float y,float width,float size,const std::string& value,
          NVGcolor color, bool right = false)
{
    if (value.empty() || width <= 0) return;
    nvgSave(vg);
    nvgIntersectScissor(vg,x,y-size,width,size*2);
    nvgFontSize(vg,size);
    nvgTextAlign(vg,(right ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg,color);
    nvgText(vg,right ? x+width : x,y,value.c_str(),nullptr);
    nvgRestore(vg);
}

class Texture {
    int id = 0;
public:
    Texture() = default;
    Texture(const Texture&) = delete;
    ~Texture() { if (id>0) nvgDeleteImage(brls::Application::getNVGContext(),id); }
    bool load(NVGcontext* vg,const std::string& path)
    {
        if (id || path.empty()) return false;
        id = nvgCreateImage(vg,path.c_str(),0);
        if (!id) id = -1;
        return true;
    }
    bool draw(NVGcontext* vg,float x,float y,float w,float h,bool cover=false) const
    {
        if (id<=0) return false;
        int iw=0, ih=0;
        nvgImageSize(vg,id,&iw,&ih);
        if (iw<=0 || ih<=0) return false;
        const float scale=cover ? std::max(w/iw,h/ih) : std::min(w/iw,h/ih);
        const float dw=iw*scale, dh=ih*scale, dx=x+(w-dw)/2, dy=y+(h-dh)/2;
        nvgBeginPath(vg);
        nvgRoundedRect(vg,cover ? x : dx,cover ? y : dy,cover ? w : dw,cover ? h : dh,4);
        nvgFillPaint(vg,nvgImagePattern(vg,dx,dy,dw,dh,0,id,1));
        nvgFill(vg);
        return true;
    }
};

// Directional repeats in Borealis follow a change of focus. Alternate these
// anchors as the logical selection moves through the virtual grid or text.
class Screen : public brls::View {
    class Anchor : public brls::View {
        void draw(NVGcontext*,int,int,unsigned,unsigned,brls::Style*,brls::FrameContext*) override {}
        brls::View* getDefaultFocus() override { return this; }
        bool isHighlightBackgroundEnabled() override { return false; }
    };
    Anchor anchors[2];
    time_t shownAt = 0;
    std::string clockText;
    bool powerService = R_SUCCEEDED(psmInitialize());
protected:
    virtual bool move(Direction d) = 0;
    virtual void paint(NVGcontext* vg,brls::FrameContext* ctx) = 0;
    void selection(brls::FrameContext* ctx, int x, int y, unsigned width, unsigned height)
    {
        auto* focus = brls::Application::getCurrentFocus();
        for (auto& anchor : anchors) {
            if (focus == &anchor) {
                anchor.setBoundaries(x, y, width, height);
                anchor.frame(ctx);
                break;
            }
        }
    }
    void summaryRow(NVGcontext* vg,const brls::Theme& theme,const std::string& summary)
    {
        const auto now = time(nullptr);
        if (shownAt != now) {
            u32 level = 0;
            struct tm local{};
            localtime_r(&now,&local);
            clockText = fmt::format("{:02}:{:02}",local.tm_hour,local.tm_min);
            if (powerService && R_SUCCEEDED(psmGetBatteryChargePercentage(&level)))
                clockText += fmt::format("    {}%",level);
            shownAt = now;
        }
        text(vg,72,24,945,19,summary,theme.descriptionColor);
        text(vg,1030,24,178,19,clockText,theme.descriptionColor,true);
    }
public:
    Screen() { for(auto& a:anchors) a.setParent(this); }
    ~Screen() { if (powerService) psmExit(); }
    brls::View* getDefaultFocus() override { return &anchors[0]; }
    brls::View* getNextFocus(Direction d,brls::View* current) override
    {
        return move(d) ? &anchors[current==&anchors[0] ? 1 : 0] : current;
    }
    void draw(NVGcontext* vg,int x,int y,unsigned w,unsigned h,brls::Style*,brls::FrameContext* ctx) override
    {
        nvgSave(vg);
        nvgTranslate(vg,x,y);
        nvgScale(vg,w/1280.f,h/560.f);
        nvgGlobalAlpha(vg,getAlpha());
        nvgFontFaceId(vg,ctx->fontStash->regular);
        paint(vg,ctx);
        nvgRestore(vg);
    }
};

// Use the same header, logo and controller hints as AIO MOD's menus.
void openScreen(const std::string& title, Screen* content)
{
    auto* frame = new brls::AppletFrame(false, false);
    frame->setTitle(title);
    frame->setIcon("romfs:/gui_icon.png");
    frame->setContentView(content);
    brls::Application::pushView(frame);
}

std::string paragraphs(std::string s)
{
    for (size_t i=0;(i=s.find("\\n",i))!=s.npos;) s.replace(i++,2,"\n");
    return s;
}

class Detail final : public Screen {
    Package pkg;
    std::shared_ptr<Pictures> icons;
    Pictures covers{true};
    Texture icon,cover;
    brls::Button installButton{brls::ButtonStyle::PRIMARY};
    bool changes = false;
    std::string body;
    std::vector<std::string_view> lines;
    size_t first = 0;
    static constexpr size_t visibleLines = 9;
    void setBody()
    {
        lines.clear();
        first = 0;
        body = paragraphs(changes ? pkg.changelog : (pkg.details.empty() ? pkg.description : pkg.details));
        if (body.empty()) body = "menus/ports/details_unknown"_i18n;
    }
    bool move(Direction dir) override
    {
        if (dir==Direction::UP && first) { --first; return true; }
        if (dir==Direction::DOWN && first+visibleLines<lines.size()) { ++first; return true; }
        return false;
    }
    void paint(NVGcontext* vg,brls::FrameContext* ctx) override
    {
        const auto& theme = *ctx->theme;
        summaryRow(vg,theme,pkg.author);
        rect(vg,918,48,332,500,theme.sidebarColor);
        icon.load(vg,icons->path(pkg.name));
        cover.load(vg,covers.path(pkg.name));
        if (!cover.draw(vg,70,48,848,177)) icon.draw(vg,70,48,848,177);
        icon.draw(vg,968,65,256,125);
        if (!pkg.details.empty() && pkg.description != pkg.details)
            text(vg,110,237,754,18,pkg.description,theme.descriptionColor);
        const std::array<std::string,5> meta = {
            tr("version")+"  "+pkg.version, tr("updated")+"  "+pkg.updated,
            tr("category")+"  "+categoryLabel(pkg.category), tr("extracted")+fmt::format("  {:.1f} MiB",pkg.installedKiB/1024.0),
            tr("downloads")+fmt::format("  {}",pkg.downloads)
        };
        for(size_t i=0;i<meta.size();++i) text(vg,948,221+i*38,285,20,meta[i],theme.textColor);
        installButton.frame(ctx);
        selection(ctx,963,463,266,58);
        if (lines.empty()) {
            const char* start=body.data();
            const char* end=start+body.size();
            NVGtextRow batch[16];
            nvgFontSize(vg,20);
            while(start<end) {
                const int count=nvgTextBreakLines(vg,start,end,754,batch,16);
                if (!count || batch[count-1].next<=start) break;
                for(int i=0;i<count;++i) lines.emplace_back(batch[i].start,batch[i].end-batch[i].start);
                start=batch[count-1].next;
            }
        }
        nvgFontSize(vg,20);
        nvgFillColor(vg,theme.textColor);
        nvgTextAlign(vg,NVG_ALIGN_LEFT|NVG_ALIGN_MIDDLE);
        for(size_t i=first;i<std::min(first+visibleLines,lines.size());++i)
            nvgText(vg,110,262+(i-first)*30,lines[i].data(),lines[i].data()+lines[i].size());
        if(lines.size()>visibleLines) {
            const float thumb=260.f*visibleLines/lines.size();
            rect(vg,880,250,5,260,theme.tableEvenBackgroundColor);
            rect(vg,880,250+(260-thumb)*first/(lines.size()-visibleLines),5,thumb,theme.highlightColor2);
        }
    }
public:
    Detail(const Package& p,std::shared_ptr<Pictures> images) : pkg(p),icons(std::move(images))
    {
        installButton.setLabel(tr("install"));
        installButton.setParent(this);
        installButton.setBoundaries(963,463,266,58);
        covers.request({p.name});
        setBody();
        registerAction(tr("install"),brls::Key::A,[this]{ installPrompt(pkg); return true; });
        if (!pkg.changelog.empty()) {
            registerAction("menus/changelog/changelog"_i18n,brls::Key::L,[this]{
                changes=!changes;
                setBody();
                updateActionHint(brls::Key::L,changes ? tr("details") : "menus/changelog/changelog"_i18n);
                return true;
            });
        }
    }
};

class Browser final : public Screen {
    enum class Status { Available, Installed, Local, Updated };
    enum MenuRow { Filter, Sort, Order, Layout, Search, Refresh, Count };
    std::shared_ptr<Catalog> catalog;
    std::shared_ptr<Pictures> pictures = std::make_shared<Pictures>(false);
    std::map<std::string,Texture> textures;
    std::map<std::string,Status> status;
    std::vector<const Package*> entries;
    std::vector<std::string> categories;
    std::string category, query;
    size_t selected=0, top=0;
    int sort=0, option=Filter;
    std::vector<std::string> choices;
    int choice=0;
    bool descending=true, grid=true, options=false;

    size_t columns() const { return grid ? 3 : 1; }
    size_t capacity() const { return grid ? 9 : 6; }

    void window(bool force=false)
    {
        const size_t previous=top;
        if (selected<top) top=selected/columns()*columns();
        if (selected>=top+capacity()) top=selected/columns()*columns()+columns()-capacity();
        if (previous==top && !force) return;
        std::deque<std::string> names;
        for(size_t i=top;i<std::min(top+capacity(),entries.size());++i) names.push_back(entries[i]->name);
        std::erase_if(textures,[&](const auto& v){
            return std::find(names.begin(),names.end(),v.first)==names.end();
        });
        pictures->request(std::move(names));
    }

    static int dateKey(const std::string& s)
    {
        int d=0,m=0,y=0;
        return sscanf(s.c_str(),"%d/%d/%d",&d,&m,&y)==3 ? y*10000+m*100+d : 0;
    }

    void updatePosition()
    {
        if (auto* frame=static_cast<brls::AppletFrame*>(getParent()))
            frame->setFooterText(fmt::format("{} / {}",entries.empty()?0:selected+1,entries.size()));
    }

    void rebuild()
    {
        entries.clear();
        for(const auto& p : catalog->packages) {
            if (!category.empty() && category!=p.category) continue;
            auto matches=[this](const std::string& s){
                return std::search(s.begin(),s.end(),query.begin(),query.end(),
                    [](unsigned char a,unsigned char b){ return std::tolower(a)==std::tolower(b); })!=s.end();
            };
            if (query.empty() || matches(p.title) || matches(p.name) || matches(p.author))
                entries.push_back(&p);
        }
        std::sort(entries.begin(),entries.end(),[this](auto* a,auto* b){
            int cmp=0;
            if (sort==0) {
                auto l=dateKey(a->updated), r=dateKey(b->updated);
                cmp=(l>r)-(l<r);
            } else if (sort==2) cmp=(a->downloads>b->downloads)-(a->downloads<b->downloads);
            else cmp=strcasecmp(a->title.c_str(),b->title.c_str());
            if (cmp) return descending ? cmp>0 : cmp<0;
            return a->name<b->name;
        });
        selected=top=0;
        window(true);
        updatePosition();
    }

    void reload(std::shared_ptr<Catalog> fresh)
    {
        catalog=std::move(fresh);
        std::set<std::string> available;
        for(const auto& p:catalog->packages) if (!p.category.empty()) available.insert(p.category);
        categories.assign(available.begin(),available.end());
        if (!available.contains(category)) category.clear();
        status.clear();
        rebuild();
    }
    void refresh()
    {
        auto result=std::make_shared<Catalog>();
        (new CatalogRefreshDialog([result]{ *result=load(); },[this,result]{
            if (!result->packages.empty()) reload(result);
            else catalog->offline=true;
        }))->open();
    }
    void search()
    {
        if (brls::Swkbd::openForText([this](std::string value){query=std::move(value);},
            "menus/apps/search_title"_i18n,"",80,query,0,"menus/apps/search_apply"_i18n,
            "menus/apps/search_placeholder"_i18n)) {
            options=false;
            rebuild();
        }
    }

    static Status inspect(const Package& p)
    {
        try {
            const bool hasBinary = !p.binary.empty() && p.binary!="none";
            if (hasBinary && (p.binary.front()!='/' || p.binary.find("..")!=p.binary.npos ||
                !std::filesystem::is_regular_file(p.binary))) return Status::Available;
            const auto path="/switch/appstore/.get/packages/"+p.name+"/info.json";
            std::error_code ec;
            if (std::filesystem::file_size(path,ec)<=256*1024 && !ec) {
                std::ifstream input(path);
                auto info=nlohmann::json::parse(input,nullptr,false);
                if (info.is_object() && info.contains("version") && info["version"].is_string())
                    return info["version"]==p.version ? Status::Installed : Status::Updated;
            }
            return hasBinary ? Status::Local : Status::Available;
        } catch(const std::exception&) { return Status::Available; }
    }

    bool move(Direction d) override
    {
        if(options) {
            int& cursor=choices.empty() ? option : choice;
            int last=choices.empty() ? int(Count)-1 : int(choices.size())-1;
            int next=std::clamp(cursor+(d==Direction::UP ? -1 : d==Direction::DOWN ? 1 : 0),0,last);
            if (cursor==next) return false;
            cursor=next;
            return true;
        }
        if(entries.empty()) return false;
        const size_t before=selected;
        if(d==Direction::DOWN && (selected/columns()+1)*columns()<entries.size())
            selected=std::min(selected+columns(),entries.size()-1);
        if(d==Direction::UP && selected>=columns()) selected-=columns();
        if(d==Direction::LEFT && selected%columns()) --selected;
        if(d==Direction::RIGHT && selected%columns()+1<columns() && selected+1<entries.size()) ++selected;
        window();
        if (selected!=before) updatePosition();
        return selected!=before;
    }
    std::vector<std::string> sorts() const { return {tr("updated"),tr("name"),tr("downloads")}; }
    std::array<std::string,Count> labels() const {
        return {tr("filter"),tr("sort"),tr("order"),tr("layout"),"menus/apps/search"_i18n,"menus/common/catalog_refresh"_i18n};
    }
    void pick()
    {
        if(!options) {
            if(!entries.empty()) {
                const auto& pkg=*entries[selected];
                openScreen(pkg.title,new Detail(pkg,pictures));
            }
            return;
        }
        if(option==Search) { search(); return; }
        if(option==Refresh) { options=false; refresh(); return; }
        if (!choices.empty()) {
            if(option==Filter) category=choice ? categories.at(choice-1) : "";
            if(option==Sort) sort=choice;
            if(option==Order) descending=(choice==0);
            if(option==Layout) grid=(choice==0);
            choices.clear();
            rebuild();
            return;
        }
        choice=0;
        if(option==Filter) {
            choices.clear();
            for (const auto& id : categories) choices.push_back(categoryLabel(id));
            choices.insert(choices.begin(),tr("all"));
            if (!category.empty()) choice=1+(std::find(categories.begin(),categories.end(),category)-categories.begin());
        } else if(option==Sort) { choices=sorts(); choice=sort; }
        else if(option==Order) { choices={tr("descending"),tr("ascending")}; choice=descending?0:1; }
        else { choices={tr("grid"),tr("list")}; choice=grid?0:1; }
    }

    void paint(NVGcontext* vg,brls::FrameContext* ctx) override
    {
        const auto& theme = *ctx->theme;
        const auto filtering=category.empty()?tr("all"):categoryLabel(category);
        const auto order=descending?tr("descending"):tr("ascending");
        const auto sorted=sorts()[sort];
        auto summary=tr("filter")+": "+filtering+"  |  "+tr("sort")+": "+sorted+"  |  "+order;
        if (!query.empty()) summary+="  |  "+query;
        if (catalog->offline && !catalog->packages.empty()) summary="menus/hb_store/offline"_i18n+"  |  "+summary;
        summaryRow(vg,theme,summary);
        std::string message;
        if(catalog->packages.empty()) message="menus/apps/load_failed"_i18n;
        else if(entries.empty()) message="menus/apps/no_results"_i18n;
        if (!message.empty()) {
            nvgFontSize(vg,24);
            nvgFillColor(vg,theme.textColor);
            nvgTextAlign(vg,NVG_ALIGN_CENTER|NVG_ALIGN_TOP);
            nvgTextBox(vg,190,290,900,message.c_str(),nullptr);
        }
        bool uploaded=false, checked=false;
        for(size_t i=top;i<std::min(entries.size(),top+capacity());++i) {
            const auto& p=*entries[i];
            auto& image=textures[p.name];
            if (!uploaded) uploaded=image.load(vg,pictures->path(p.name));
            if (!checked && !status.contains(p.name)) { status[p.name]=inspect(p); checked=true; }
            const size_t slot=i-top;
            const float x=74+(slot%columns())*380, y=53+(slot/columns())*(grid?166:83);
            const float w=grid?370:1130, h=grid?154:76;
            const float img=grid?115:66, ix=x+(grid?20:8), iy=y+(h-img)/2;
            if(i==selected) rect(vg,x,y,w,h,theme.highlightBackgroundColor);
            else if(grid) rect(vg,x,y,w,h,theme.tableEvenBackgroundColor);
            else rect(vg,x,y+h-1,w,1,theme.listItemSeparatorColor);
            rect(vg,ix,iy,img,img,theme.backgroundColorRGB);
            if (!image.draw(vg,ix,iy,img,img,true)) text(vg,ix+8,iy+img/2,img-12,16,"HB",theme.descriptionColor);
            const auto color=theme.textColor;
            float tx=ix+img+12, space=x+w-tx-14;
            text(vg,tx,y+(grid?40:24),space,21,p.title,color);
            text(vg,tx,y+(grid?77:56),grid?space:space*.6f,19,p.author,theme.descriptionColor);
            text(vg,grid?tx:x+w-245,y+(grid?112:41),grid?space-25:196,19,p.version,theme.listItemValueColor);
            auto known=status.find(p.name);
            if(known!=status.end()) {
                auto state=known->second;
                const float cx=x+w-20, cy=y+h-20;
                nvgBeginPath(vg); nvgCircle(vg,cx,cy,10);
                const bool filled = state == Status::Installed || state == Status::Updated;
                nvgFillColor(vg, filled ? theme.buttonPrimaryEnabledBackgroundColor : theme.tableEvenBackgroundColor);
                nvgFill(vg);
                if (!filled) {
                    nvgStrokeWidth(vg,1.5f);
                    nvgStrokeColor(vg,theme.descriptionColor);
                    nvgStroke(vg);
                }
                nvgBeginPath(vg);
                if(state==Status::Available || state==Status::Updated) {
                    const int sign=state==Status::Available ? 1 : -1;
                    nvgMoveTo(vg,cx,cy-5*sign); nvgLineTo(vg,cx,cy+4*sign);
                    nvgMoveTo(vg,cx-4,cy); nvgLineTo(vg,cx,cy+4*sign); nvgLineTo(vg,cx+4,cy);
                } else {
                    nvgMoveTo(vg,cx-5,cy); nvgLineTo(vg,cx-2,cy+3); nvgLineTo(vg,cx+5,cy-4);
                }
                nvgStrokeWidth(vg,1.8f); nvgStrokeColor(vg,filled ? theme.buttonPrimaryEnabledTextColor : theme.textColor); nvgStroke(vg);
            }
            if(i==selected && !options) selection(ctx,x,y,w,h);
        }
        if(!options) return;
        rect(vg,0,0,1280,560,theme.backdropColor);
        rect(vg,830,0,450,560,theme.sidebarColor);
        auto names=labels();
        text(vg,859,29,386,25,choices.empty()?tr("options"):names[option],theme.textColor);
        rect(vg,846,55,418,1,theme.separatorColor);
        const std::array<std::string,Count> values={filtering,sorted,order,grid?tr("grid"):tr("list"),query,""};
        const int cursor=choices.empty()?option:choice;
        const int count=choices.empty()?int(Count):int(choices.size());
        const int start=std::max(0,cursor-5);
        for(int i=start;i<std::min(count,start+6);++i) {
            const float y=68+(i-start)*77;
            if(i==cursor) rect(vg,854,y,395,66,theme.highlightBackgroundColor);
            else rect(vg,854,y+65,395,1,theme.listItemSeparatorColor);
            if(choices.empty()) {
                text(vg,873,y+33,values[i].empty()?350:180,22,names[i],theme.textColor);
                text(vg,1060,y+33,169,21,values[i],theme.highlightColor2,true);
            } else text(vg,873,y+33,350,22,choices[i],i==cursor?theme.highlightColor2:theme.textColor);
            if(i==cursor) selection(ctx,854,y,395,66);
        }
    }
public:
    void willAppear(bool resetState = false) override
    {
        brls::View::willAppear(resetState);
        updatePosition();
        if (!entries.empty()) status.erase(entries[selected]->name);
    }
    explicit Browser(std::shared_ptr<Catalog> initial)
    {
        reload(std::move(initial));
        registerAction(tr("options"),brls::Key::X,[this]{ choices.clear(); options=!options; return true; });
        registerAction("menus/apps/search"_i18n,brls::Key::Y,[this]{ choices.clear(); search(); return true; });
        registerAction("brls/hints/ok"_i18n,brls::Key::A,[this]{ pick(); return true; });
        registerAction("brls/hints/back"_i18n,brls::Key::B,[this]{
            if(!choices.empty()) choices.clear();
            else if(options) options=false;
            else brls::Application::popView();
            return true;
        });
    }
};
} // namespace

void open()
{
    auto result = std::make_shared<Catalog>();
    (new CatalogRefreshDialog([result]{ *result = load(); },[result]{
        openScreen(tr("title"),new Browser(result));
    }))->open();
}
} // namespace hb_store
