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
#include <winrt/Microsoft.UI.Windowing.h>
#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>
#include <chrono>
#include <functional>
#include <memory>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Windows::Foundation;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace viewer;

namespace {
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
    ToolTip hover;
    ComboBox mode, uvChannel;
    Flyout settingsFlyout;
    DispatcherTimer timer;
    bool ready{}, loading{}, closed{}, dirty{true};
    uint64_t loadGeneration{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(), lastHover{};
    struct Drag {
        bool active{}, pan{};
        Point last{};
    } drag3d, dragUV;

    void report(hstring const &text) {
        status.Text(text);
    }
    void fail(hstring const &text) {
        report(L"エラー: " + text);
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
            settings.mode = static_cast<Mode>(mode.SelectedIndex());
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
            settings.uvChannel = uvChannel.SelectedIndex();
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
        hover.Placement(PlacementMode::Mouse);
        hover.IsHitTestVisible(false);
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
            hover.IsOpen(false);
        });
        refreshSettings();
        window.Content(root);
        window.Activate();
    }
    static double Auto() {
        return 1;
    }
    void resize() {
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
                hover.IsOpen(false);
                e.Handled(true);
            });
        panel.PointerMoved(
            [this, &renderer, &drag, panel](auto const &, Input::PointerRoutedEventArgs const &e) {
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
                    hover.Content(box_value(describeVertex(model->vertices[*hit], *hit, *model)));
                    hover.PlacementTarget(panel);
                    hover.IsOpen(true);
                } else
                    hover.IsOpen(false);
            });
        panel.PointerReleased([&drag, panel](auto const &, Input::PointerRoutedEventArgs const &) {
            drag.active = false;
            panel.ReleasePointerCaptures();
        });
        panel.PointerCaptureLost([&drag](auto const &, auto const &) { drag.active = false; });
        panel.PointerExited([this](auto const &, auto const &) { hover.IsOpen(false); });
        panel.PointerWheelChanged(
            [this, &renderer, panel](auto const &, Input::PointerRoutedEventArgs const &e) {
                renderer.zoom(static_cast<float>(e.GetCurrentPoint(panel).Properties().MouseWheelDelta()));
                hover.IsOpen(false);
                dirty = true;
                e.Handled(true);
            });
        panel.DoubleTapped([this, &renderer](auto const &, auto const &) {
            renderer.fit();
            dirty = true;
        });
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
                settings.attribute = static_cast<Attribute>(sender.template as<ComboBox>().SelectedIndex());
                refreshColors();
            });
            content.Children().Append(attribute);
            ComboBox channel;
            channel.Header(box_value(L"TEXCOORD / COLOR 番号"));
            for (int i = 0; i < 8; ++i)
                channel.Items().Append(box_value(to_hstring(i)));
            channel.SelectedIndex(settings.attributeChannel);
            channel.SelectionChanged([this](auto const &sender, auto const &) {
                settings.attributeChannel = sender.template as<ComboBox>().SelectedIndex();
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
        hover.IsOpen(false);
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
struct App : ApplicationT<App> {
    std::shared_ptr<ViewerWindow> viewer;
    App() {
        Resources().MergedDictionaries().Append(XamlControlsResources());
    }
    void OnLaunched(LaunchActivatedEventArgs const &) {
        viewer = std::make_shared<ViewerWindow>();
        viewer->initialize();
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        init_apartment(apartment_type::single_threaded);
        Application::Start([](auto &&) { make<App>(); });
        return 0;
    } catch (hresult_error const &e) {
        MessageBoxW(nullptr, e.message().c_str(), L"FBX Viewer", MB_OK | MB_ICONERROR);
        return 1;
    } catch (std::exception const &e) {
        auto message = to_hstring(e.what());
        MessageBoxW(nullptr, message.c_str(), L"FBX Viewer", MB_OK | MB_ICONERROR);
        return 1;
    }
}
