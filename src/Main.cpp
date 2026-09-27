#include "Renderer.h"
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>
#include <chrono>
#include <functional>
#include <memory>
#include <fstream>
#include <algorithm>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Windows::Foundation;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace viewer;

namespace {
bool smokeTest{};
bool hoverTest{};
int exitStatus{};
void startupLog(std::wstring const &message) {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    std::ofstream log(std::filesystem::path(executable).parent_path() / L"startup.log", std::ios::app);
    log << to_string(message) << '\n';
}
TextBlock label(hstring const &value, double size = 14) {
    TextBlock t;
    t.Text(value);
    t.FontSize(size);
    return t;
}
Media::SolidColorBrush brush(uint8_t r, uint8_t g, uint8_t b) {
    return Media::SolidColorBrush(Windows::UI::Color{255, r, g, b});
}
struct ViewerWindow : std::enable_shared_from_this<ViewerWindow> {
    Window window;
    Grid root, views;
    SwapChainPanel left, right;
    Renderer render3d, renderUV;
    Settings settings;
    std::shared_ptr<Model> model;
    TextBlock status = label(L"FBXファイルを開くか、このウィンドウへドロップしてください。"),
              empty = label(L"FBX VIEWER", 28), uvNotice = label(L"UV / TEXCOORD0", 12);
    Canvas hoverLayer;
    Border hover;
    TextBlock hoverText;
    ComboBox mode, uvChannel;
    Flyout settingsFlyout;
    DispatcherTimer timer;
    bool ready{}, loading{}, closed{}, dirty{true};
    uint64_t loadGeneration{};
    unsigned hoverTestStep{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(), lastHover{};
    struct Drag {
        bool active{}, pan{};
        Point last{};
    } drag3d, dragUV;

    void report(hstring const &text) {
        status.Text(text);
    }
    void fail(hstring const &text) {
        startupLog(L"Error: " + std::wstring(text));
        report(L"エラー: " + text);
        if (smokeTest || hoverTest)
            window.Close();
    }
    template <class F> void guarded(F action) {
        try {
            action();
        } catch (hresult_error const &e) {
            fail(e.message());
        } catch (std::exception const &e) {
            fail(to_hstring(e.what()));
        }
    }
    void initialize() {
        startupLog(L"Initialize window");
        window.Title(L"FBX Viewer");
        window.AppWindow().Resize({1440, 900});
        root.RequestedTheme(ElementTheme::Dark);
        root.Background(brush(19, 22, 28));
        RowDefinition toolbarRow;
        toolbarRow.Height({56, GridUnitType::Pixel});
        root.RowDefinitions().Append(toolbarRow);
        RowDefinition viewRow;
        viewRow.Height({1, GridUnitType::Star});
        root.RowDefinitions().Append(viewRow);
        RowDefinition statusRow;
        statusRow.Height({Auto(), GridUnitType::Auto});
        root.RowDefinitions().Append(statusRow);
        Grid toolbar;
        toolbar.Padding({12, 8, 12, 8});
        ColumnDefinition flexible;
        flexible.Width({1, GridUnitType::Star});
        toolbar.ColumnDefinitions().Append(flexible);
        ColumnDefinition controls;
        controls.Width({1, GridUnitType::Auto});
        toolbar.ColumnDefinitions().Append(controls);
        MenuBar menu;
        menu.VerticalAlignment(VerticalAlignment::Center);
        MenuBarItem file;
        file.Title(L"ファイル");
        MenuFlyoutItem open;
        open.Text(L"開く…");
        open.Click([this](auto const &, auto const &) { pickFile(); });
        file.Items().Append(open);
        menu.Items().Append(file);
        toolbar.Children().Append(menu);
        StackPanel tools;
        tools.Orientation(Orientation::Horizontal);
        tools.Spacing(10);
        Grid::SetColumn(tools, 1);
        mode.Items().Append(box_value(L"サーフェス"));
        mode.Items().Append(box_value(L"UVスクロール"));
        mode.Items().Append(box_value(L"頂点情報"));
        mode.SelectedIndex(0);
        mode.MinWidth(150);
        mode.SelectionChanged([this](auto const &, auto const &) {
            auto selected = mode.SelectedIndex();
            if (closed || selected < 0 || selected > 2)
                return;
            hideVertexHover();
            settings.mode = static_cast<Mode>(selected);
            refreshSettings();
            dirty = true;
        });
        tools.Children().Append(mode);
        Button settingsButton;
        settingsButton.Content(box_value(L"表示設定"));
        settingsButton.Flyout(settingsFlyout);
        tools.Children().Append(settingsButton);
        Button fit;
        fit.Content(box_value(L"全体を表示"));
        fit.Click([this](auto const &, auto const &) {
            render3d.fit();
            renderUV.fit();
            dirty = true;
        });
        tools.Children().Append(fit);
        toolbar.Children().Append(tools);
        root.Children().Append(toolbar);
        ColumnDefinition l;
        l.Width({1, GridUnitType::Star});
        views.ColumnDefinitions().Append(l);
        ColumnDefinition gap;
        gap.Width({4, GridUnitType::Pixel});
        views.ColumnDefinitions().Append(gap);
        ColumnDefinition r;
        r.Width({1, GridUnitType::Star});
        views.ColumnDefinitions().Append(r);
        views.Children().Append(left);
        Grid::SetColumn(right, 2);
        views.Children().Append(right);
        Grid::SetRow(views, 1);
        root.Children().Append(views);
        auto title =
            label(L"3D VIEW   ·   左ドラッグ: 回転   /   右・中ドラッグ: 移動   /   ホイール: 拡大縮小", 12);
        title.Margin({14, 12, 14, 0});
        title.VerticalAlignment(VerticalAlignment::Top);
        title.IsHitTestVisible(false);
        views.Children().Append(title);
        StackPanel uvTools;
        uvTools.Orientation(Orientation::Horizontal);
        uvTools.Spacing(10);
        uvTools.Margin({14, 8, 14, 0});
        uvTools.VerticalAlignment(VerticalAlignment::Top);
        Grid::SetColumn(uvTools, 2);
        uvNotice.VerticalAlignment(VerticalAlignment::Center);
        uvTools.Children().Append(uvNotice);
        for (int i = 0; i < 8; ++i)
            uvChannel.Items().Append(box_value(L"TEXCOORD" + to_hstring(i)));
        uvChannel.SelectedIndex(0);
        uvChannel.MinWidth(150);
        uvChannel.SelectionChanged([this](auto const &, auto const &) {
            auto selected = uvChannel.SelectedIndex();
            if (closed || selected < 0 || selected >= 8)
                return;
            hideVertexHover();
            settings.uvChannel = selected;
            refreshColors();
            updateUVNotice();
        });
        uvTools.Children().Append(uvChannel);
        views.Children().Append(uvTools);
        empty.HorizontalAlignment(HorizontalAlignment::Center);
        empty.VerticalAlignment(VerticalAlignment::Center);
        empty.IsHitTestVisible(false);
        Grid::SetColumnSpan(empty, 3);
        views.Children().Append(empty);
        // Keep vertex information in the same visual tree as the viewports.
        // A manually opened, unattached ToolTip has no reliable XamlRoot/owner.
        hoverLayer.IsHitTestVisible(false);
        Grid::SetColumnSpan(hoverLayer, 3);
        hover.Background(brush(30, 36, 46));
        hover.BorderBrush(brush(75, 85, 100));
        hover.BorderThickness({1, 1, 1, 1});
        hover.CornerRadius({6, 6, 6, 6});
        hover.Padding({10, 10, 10, 10});
        hoverText.FontSize(12);
        hoverText.TextWrapping(TextWrapping::Wrap);
        hoverText.Foreground(brush(235, 239, 245));
        hover.Child(hoverText);
        hideVertexHover();
        hoverLayer.Children().Append(hover);
        views.Children().Append(hoverLayer);
        status.Margin({14, 8, 14, 8});
        status.FontSize(12);
        status.TextWrapping(TextWrapping::Wrap);
        Grid::SetRow(status, 2);
        root.Children().Append(status);
        root.AllowDrop(true);
        root.DragOver([](auto const &, DragEventArgs const &e) {
            e.AcceptedOperation(e.DataView().Contains(StandardDataFormats::StorageItems())
                                    ? DataPackageOperation::Copy
                                    : DataPackageOperation::None);
        });
        root.Drop([this](auto const &, DragEventArgs const &e) { drop(e); });
        root.Loaded([this](auto const &, auto const &) {
            guarded([&] {
                if (ready)
                    return;
                render3d.initialize(left, false);
                renderUV.initialize(right, true);
                ready = true;
                resize();
                timer.Start();
            });
        });
        left.SizeChanged([this](auto const &, auto const &) { guarded([&] { resize(); }); });
        right.SizeChanged([this](auto const &, auto const &) { guarded([&] { resize(); }); });
        left.CompositionScaleChanged([this](auto const &, auto const &) { guarded([&] { resize(); }); });
        right.CompositionScaleChanged([this](auto const &, auto const &) { guarded([&] { resize(); }); });
        bindPointer(left, render3d, drag3d);
        bindPointer(right, renderUV, dragUV);
        timer.Interval(std::chrono::milliseconds(16));
        timer.Tick([this](auto const &, auto const &) {
            if (!ready || closed || (!dirty && settings.mode != Mode::UVScroll))
                return;
            try {
                float elapsed =
                    std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
                render3d.draw(settings, elapsed);
                renderUV.draw(settings, elapsed);
                dirty = false;
                if (hoverTest) {
                    runHoverTest();
                }
                if (smokeTest) {
                    startupLog(L"Startup smoke test passed: both viewports rendered");
                    exitStatus = 0;
                    window.Close();
                }
            } catch (hresult_error const &e) {
                timer.Stop();
                ready = false;
                fail(L"描画を停止しました。アプリを再起動してください。 " + e.message());
            }
        });
        window.Closed([this](auto const &, auto const &) {
            closed = true;
            ++loadGeneration;
            timer.Stop();
            hideVertexHover();
        });
        refreshSettings();
        window.Content(root);
        window.Activate();
    }
    static double Auto() {
        return 1;
    }
    void resize() {
        hideVertexHover();
        if (!ready)
            return;
        render3d.resize(static_cast<float>(left.ActualWidth()), static_cast<float>(left.ActualHeight()),
                        left.CompositionScaleX());
        renderUV.resize(static_cast<float>(right.ActualWidth()), static_cast<float>(right.ActualHeight()),
                        right.CompositionScaleX());
        dirty = true;
    }
    void refreshColors() {
        if (ready && model)
            guarded([&] {
                render3d.updateColors(settings);
                renderUV.updateColors(settings);
            });
        dirty = true;
    }
    void updateUVNotice() {
        uvNotice.Text(model && !(model->uvMask & (1u << settings.uvChannel)) ? L"このUVチャンネルはありません"
                                                                             : L"UV VIEW");
    }
    void restoreModel() noexcept {
        try {
            render3d.setModel(model.get(), settings);
            renderUV.setModel(model.get(), settings);
        } catch (...) {
            render3d.setModel(nullptr, settings);
            renderUV.setModel(nullptr, settings);
            model.reset();
            empty.Visibility(Visibility::Visible);
        }
        dirty = true;
    }
    void bindPointer(SwapChainPanel const &panel, Renderer &renderer, Drag &drag) {
        panel.PointerPressed(
            [this, &renderer, &drag, panel](auto const &, Input::PointerRoutedEventArgs const &e) {
                auto p = e.GetCurrentPoint(panel);
                auto props = p.Properties();
                if (!(props.IsLeftButtonPressed() || props.IsRightButtonPressed() ||
                      props.IsMiddleButtonPressed()))
                    return;
                drag = {true, props.IsRightButtonPressed() || props.IsMiddleButtonPressed(), p.Position()};
                panel.CapturePointer(e.Pointer());
                hideVertexHover();
                e.Handled(true);
            });
        panel.PointerMoved(
            [this, &renderer, &drag, panel](auto const &, Input::PointerRoutedEventArgs const &e) {
                if (closed || !ready)
                    return;
                guarded([&] {
                    auto p = e.GetCurrentPoint(panel).Position();
                    if (drag.active) {
                        float dx = p.X - drag.last.X, dy = p.Y - drag.last.Y;
                        if (drag.pan)
                            renderer.pan(dx, dy);
                        else
                            renderer.orbit(dx, dy);
                        drag.last = p;
                        dirty = true;
                        return;
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (now - lastHover < std::chrono::milliseconds(70))
                        return;
                    lastHover = now;
                    auto hit = renderer.pick(p.X, p.Y, settings);
                    if (hit && model) {
                        showVertexHover(panel, p, *hit);
                    } else
                        hideVertexHover();
                });
            });
        panel.PointerReleased([&drag, panel](auto const &, Input::PointerRoutedEventArgs const &) {
            drag.active = false;
            panel.ReleasePointerCaptures();
        });
        panel.PointerCaptureLost([&drag](auto const &, auto const &) { drag.active = false; });
        panel.PointerExited([this](auto const &, auto const &) { hideVertexHover(); });
        panel.PointerWheelChanged(
            [this, &renderer, panel](auto const &, Input::PointerRoutedEventArgs const &e) {
                renderer.zoom(static_cast<float>(e.GetCurrentPoint(panel).Properties().MouseWheelDelta()));
                hideVertexHover();
                dirty = true;
                e.Handled(true);
            });
        panel.DoubleTapped([this, &renderer](auto const &, auto const &) {
            renderer.fit();
            dirty = true;
        });
    }
    void hideVertexHover() {
        hover.Visibility(Visibility::Collapsed);
    }
    void showVertexHover(SwapChainPanel const &panel, Point position, size_t index) {
        if (closed || !ready || !model || index >= model->vertices.size() || !panel.IsLoaded()) {
            hideVertexHover();
            return;
        }
        auto anchor = panel.TransformToVisual(views).TransformPoint(position);
        float width = static_cast<float>(views.ActualWidth());
        float height = static_cast<float>(views.ActualHeight());
        if (width < 32 || height < 32) {
            hideVertexHover();
            return;
        }
        hoverText.Text(describeVertex(model->vertices[index], index, *model));
        hover.MaxWidth(std::min(460.f, width - 16));
        hover.MaxHeight(height - 16);
        hover.Visibility(Visibility::Visible);
        hover.Measure({static_cast<float>(hover.MaxWidth()), static_cast<float>(hover.MaxHeight())});
        auto size = hover.DesiredSize();
        float x = anchor.X + 14, y = anchor.Y + 14;
        if (x + size.Width > width - 8)
            x = anchor.X - size.Width - 14;
        if (y + size.Height > height - 8)
            y = anchor.Y - size.Height - 14;
        Canvas::SetLeft(hover, std::clamp(x, 8.f, std::max(8.f, width - size.Width - 8)));
        Canvas::SetTop(hover, std::clamp(y, 8.f, std::max(8.f, height - size.Height - 8)));
    }
    void runHoverTest() {
        if (hoverTestStep == 0) {
            model = std::make_shared<Model>();
            model->vertices.resize(3);
            model->vertices[0].position = {-1, -1, 0};
            model->vertices[1].position = {1, -1, 0};
            model->vertices[2].position = {0, 1, 0};
            model->vertices[0].uv[0] = {0.5f, 0.5f, 0};
            model->vertices[1].uv[0] = {0.8f, 0.5f, 0};
            model->vertices[2].uv[0] = {0.5f, 0.8f, 0};
            for (auto &vertex : model->vertices) {
                vertex.normal = {0, 0, 1};
                vertex.uvMask = 1;
            }
            model->uvMask = 1;
            model->indices = {0, 1, 2};
            model->minimum = {-1, -1, 0};
            model->maximum = {1, 1, 0};
            model->radius = 1.5f;
            render3d.setModel(model.get(), settings);
            renderUV.setModel(model.get(), settings);
            empty.Visibility(Visibility::Collapsed);
        } else {
            if (hoverTestStep % 10 == 0)
                mode.SelectedIndex((hoverTestStep / 10) % 3);
            if (hoverTestStep % 3 == 0) {
                hideVertexHover();
                if (hover.Visibility() != Visibility::Collapsed)
                    throw hresult_error(E_FAIL, L"Hover did not close");
            } else {
                auto const &panel = hoverTestStep % 2 ? right : left;
                auto middle = Point{static_cast<float>(right.ActualWidth() / 2),
                                    static_cast<float>(right.ActualHeight() / 2)};
                auto hit = renderUV.pick(middle.X, middle.Y, settings);
                if (!hit || *hit != 0)
                    throw hresult_error(E_FAIL, L"Hover picking failed");
                auto position = Point{static_cast<float>(panel.ActualWidth() - 1),
                                      static_cast<float>(panel.ActualHeight() - 1)};
                showVertexHover(panel, position, *hit);
                if (hover.Visibility() != Visibility::Visible)
                    throw hresult_error(E_FAIL, L"Hover did not open");
                if (std::wstring(hoverText.Text()).find(L"TEXCOORD0") == std::wstring::npos)
                    throw hresult_error(E_FAIL, L"Hover attributes missing");
                auto size = hover.DesiredSize();
                if (Canvas::GetLeft(hover) < 0 || Canvas::GetTop(hover) < 0 ||
                    Canvas::GetLeft(hover) + size.Width > views.ActualWidth() ||
                    Canvas::GetTop(hover) + size.Height > views.ActualHeight())
                    throw hresult_error(E_FAIL, L"Hover outside view bounds");
            }
        }
        ++hoverTestStep;
        dirty = true;
        if (hoverTestStep > 60) {
            showVertexHover(left, {10, 10}, model->vertices.size());
            if (hover.Visibility() != Visibility::Collapsed)
                throw hresult_error(E_FAIL, L"Invalid vertex left stale hover");
            showVertexHover(right, {10, 10}, 0);
            startupLog(L"Hover regression passed: 60 frames, both views, modes, bounds, invalid vertex and "
                       L"close while visible");
            exitStatus = 0;
            window.Close();
        }
    }
    void refreshSettings() {
        StackPanel content;
        content.Spacing(10);
        content.Width(300);
        content.Children().Append(label(L"表示設定", 20));
        auto check = [&](hstring const &name, bool value, std::function<void(bool)> change) {
            CheckBox c;
            c.Content(box_value(name));
            c.IsChecked(value);
            c.Click([change](auto const &sender, auto const &) {
                change(sender.template as<CheckBox>().IsChecked().Value());
            });
            content.Children().Append(c);
        };
        auto slider = [&](hstring const &name, double value, double min, double max,
                          std::function<void(float)> change) {
            content.Children().Append(label(name, 12));
            Slider c;
            c.Minimum(min);
            c.Maximum(max);
            c.Value(value);
            c.StepFrequency((max - min) / 100);
            c.ValueChanged([change](auto const &, RangeBaseValueChangedEventArgs const &e) {
                change(static_cast<float>(e.NewValue()));
            });
            content.Children().Append(c);
        };
        check(L"頂点を表示", settings.showVertices, [this](bool value) {
            settings.showVertices = value;
            dirty = true;
        });
        slider(L"頂点サイズ", settings.pointSize, 1, 20, [this](float v) {
            settings.pointSize = v;
            dirty = true;
        });
        Button colorButton;
        colorButton.Content(box_value(L"頂点色"));
        Flyout colorFlyout;
        ColorPicker color;
        color.IsAlphaEnabled(false);
        color.IsColorSpectrumVisible(true);
        color.Color({255, static_cast<uint8_t>(settings.pointColor.r * 255),
                     static_cast<uint8_t>(settings.pointColor.g * 255),
                     static_cast<uint8_t>(settings.pointColor.b * 255)});
        color.ColorChanged([this](auto const &, ColorChangedEventArgs const &e) {
            auto c = e.NewColor();
            settings.pointColor = {c.R / 255.f, c.G / 255.f, c.B / 255.f, 1};
            dirty = true;
        });
        colorFlyout.Content(color);
        colorButton.Flyout(colorFlyout);
        content.Children().Append(colorButton);
        if (settings.mode == Mode::Surface)
            check(L"透過（深度バッファ無効）", settings.transparent, [this](bool v) {
                settings.transparent = v;
                dirty = true;
            });
        if (settings.mode == Mode::UVScroll) {
            slider(L"スクロール方向 U", settings.direction.x, -1, 1,
                   [this](float v) { settings.direction.x = v; });
            slider(L"スクロール方向 V", settings.direction.y, -1, 1,
                   [this](float v) { settings.direction.y = v; });
            slider(L"移動量 / 秒", settings.speed, 0, 2, [this](float v) { settings.speed = v; });
            slider(L"グリッド数", settings.grid, 1, 100, [this](float v) { settings.grid = v; });
        }
        if (settings.mode == Mode::Attribute) {
            ComboBox attribute;
            attribute.Header(box_value(L"頂点情報"));
            attribute.HorizontalAlignment(HorizontalAlignment::Stretch);
            for (auto name : {L"Normal → RGB", L"Position → RGB", L"TEXCOORD", L"頂点 Index",
                              L"Tangent → RGB", L"Bitangent → RGB", L"COLOR"})
                attribute.Items().Append(box_value(name));
            attribute.SelectedIndex(static_cast<int>(settings.attribute));
            attribute.SelectionChanged([this](auto const &sender, auto const &) {
                auto selected = sender.template as<ComboBox>().SelectedIndex();
                if (closed || selected < 0 || selected > 6)
                    return;
                settings.attribute = static_cast<Attribute>(selected);
                refreshColors();
            });
            content.Children().Append(attribute);
            ComboBox channel;
            channel.Header(box_value(L"TEXCOORD / COLOR 番号"));
            for (int i = 0; i < 8; ++i)
                channel.Items().Append(box_value(to_hstring(i)));
            channel.SelectedIndex(settings.attributeChannel);
            channel.SelectionChanged([this](auto const &sender, auto const &) {
                auto selected = sender.template as<ComboBox>().SelectedIndex();
                if (closed || selected < 0 || selected >= 8)
                    return;
                settings.attributeChannel = selected;
                refreshColors();
            });
            content.Children().Append(channel);
            check(L"無彩色の明るさで表示", settings.grayscale, [this](bool v) {
                settings.grayscale = v;
                refreshColors();
            });
            auto note = label(
                L"欠落属性はマゼンタ。法線等は −1～1 → 0～1、位置は境界範囲、UVは0～1に制限して表示。", 12);
            note.TextWrapping(TextWrapping::Wrap);
            content.Children().Append(note);
        }
        ScrollViewer scroll;
        scroll.MaxHeight(650);
        scroll.Content(content);
        settingsFlyout.Content(scroll);
    }
    void pickFile() {
        guarded([&] {
            com_ptr<IFileOpenDialog> dialog;
            check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(dialog.put())));
            COMDLG_FILTERSPEC filters[] = {{L"FBX model", L"*.fbx"}};
            check_hresult(dialog->SetFileTypes(1, filters));
            DWORD options{};
            check_hresult(dialog->GetOptions(&options));
            check_hresult(
                dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM));
            HWND hwnd{};
            check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
            auto hr = dialog->Show(hwnd);
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                return;
            check_hresult(hr);
            com_ptr<IShellItem> item;
            check_hresult(dialog->GetResult(item.put()));
            PWSTR raw{};
            check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &raw));
            std::filesystem::path path(raw);
            CoTaskMemFree(raw);
            load(path);
        });
    }
    fire_and_forget drop(DragEventArgs e) {
        auto lifetime = shared_from_this();
        auto deferral = e.GetDeferral();
        try {
            auto items = co_await e.DataView().GetStorageItemsAsync();
            if (items.Size() != 1)
                report(L"FBXファイルを1つだけドロップしてください。");
            else if (auto file = items.GetAt(0).try_as<Windows::Storage::StorageFile>())
                load(std::filesystem::path(file.Path().c_str()));
            else
                report(L"フォルダーではなくFBXファイルを選択してください。");
        } catch (hresult_error const &error) {
            fail(error.message());
        }
        deferral.Complete();
    }
    fire_and_forget load(std::filesystem::path path) {
        if (!ready) {
            fail(L"描画の初期化が完了していません。");
            co_return;
        }
        auto lifetime = shared_from_this();
        auto generation = ++loadGeneration;
        apartment_context ui;
        loading = true;
        report(L"読み込み中: " + hstring(path.filename().wstring()));
        hideVertexHover();
        std::shared_ptr<Model> next;
        hstring error;
        co_await resume_background();
        try {
            next = std::make_shared<Model>(loadModel(path));
        } catch (std::exception const &e) {
            error = to_hstring(e.what());
        } catch (...) {
            error = L"予期しない読み込みエラー";
        }
        co_await ui;
        if (closed || generation != loadGeneration)
            co_return;
        loading = false;
        if (!next) {
            fail(error);
            co_return;
        }
        // Keep the prior model alive until both GPU uploads succeed.
        try {
            render3d.setModel(next.get(), settings);
            renderUV.setModel(next.get(), settings);
            model = std::move(next);
            if (model->uvMask && !(model->uvMask & (1u << settings.uvChannel)))
                for (int i = 0; i < 8; ++i)
                    if (model->uvMask & (1u << i)) {
                        uvChannel.SelectedIndex(i);
                        break;
                    }
            empty.Visibility(Visibility::Collapsed);
            updateUVNotice();
            dirty = true;
            window.Title(L"FBX Viewer — " + hstring(path.filename().wstring()));
            report(hstring(path.filename().wstring()) + L"  |  " + to_hstring(model->vertices.size()) +
                   L" 頂点  ·  " + to_hstring(model->indices.size() / 3) + L" 三角形  ·  " +
                   to_hstring(model->meshes.size()) +
                   L" メッシュ  |  頂点ホバー: 属性表示 / ダブルクリック: 全体表示");
        } catch (hresult_error const &e) {
            restoreModel();
            fail(e.message());
        } catch (std::exception const &e) {
            restoreModel();
            fail(to_hstring(e.what()));
        }
    }
};
struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    std::shared_ptr<ViewerWindow> viewer;
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadataProvider;
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const &type) {
        return metadataProvider.GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(hstring const &name) {
        return metadataProvider.GetXamlType(name);
    }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadataProvider.GetXmlnsDefinitions();
    }
    App() {
        UnhandledException([](auto const &, UnhandledExceptionEventArgs const &e) {
            startupLog(L"Unhandled XAML exception: " + std::wstring(e.Message()));
        });
    }
    void OnLaunched(LaunchActivatedEventArgs const &) {
        try {
            // Resource loading queries Application.Current for XAML metadata. The
            // composable App must be fully constructed before this can succeed.
            startupLog(L"Create application resources");
            Resources().MergedDictionaries().Append(XamlControlsResources());
            startupLog(L"Application resources ready");
            startupLog(L"Create viewer controls");
            viewer = std::make_shared<ViewerWindow>();
            viewer->initialize();
            startupLog(L"Window activated");
        } catch (hresult_error const &e) {
            startupLog(L"Launch failed: " + std::wstring(e.message()));
            exitStatus = 1;
            if (!smokeTest)
                MessageBoxW(nullptr, e.message().c_str(), L"FBX Viewer — 起動エラー", MB_OK | MB_ICONERROR);
            Exit();
        }
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    try {
        smokeTest = std::wstring_view(commandLine) == L"--smoke-test";
        hoverTest = std::wstring_view(commandLine) == L"--hover-test";
        exitStatus = smokeTest || hoverTest ? 1 : 0;
        init_apartment(apartment_type::single_threaded);
        startupLog(L"Application::Start");
        Application::Start([](auto &&) { make<App>(); });
        return exitStatus;
    } catch (hresult_error const &e) {
        startupLog(L"Startup failed: " + std::wstring(e.message()));
        if (!smokeTest)
            MessageBoxW(nullptr, e.message().c_str(), L"FBX Viewer", MB_OK | MB_ICONERROR);
        return 1;
    } catch (std::exception const &e) {
        auto message = to_hstring(e.what());
        startupLog(L"Startup failed: " + std::wstring(message));
        if (!smokeTest)
            MessageBoxW(nullptr, message.c_str(), L"FBX Viewer", MB_OK | MB_ICONERROR);
        return 1;
    }
}
