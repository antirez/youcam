// YouCam Capture Exclusion: windows that ask to be hidden from recordings are
// painted on screen but skipped when Mutter repaints the stage for a screen
// cast (RecordArea), the way macOS's ScreenCaptureKit excludes YouCam's panel.
//
// A window asks by its X11 role (WM_WINDOW_ROLE) or, for Wayland test
// clients, its title starting with HIDDEN_MARK.

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const HIDDEN_MARK = 'youcam-hidden';
const EFFECT_NAME = 'youcam-capture-exclusion';

// The framebuffers of the stage view being painted on screen, set only while
// the stage paints it ("paint-view"). A screen cast paints the stage again
// into its own framebuffer (even from inside the view paint, for RecordArea),
// and screenshots paint outside it: neither matches.
let screenFramebuffers = [];

// Prototype check: YOUCAM_EXCLUSION_DEBUG=1 logs how many paints went to the
// screen and how many were skipped (screen casts, screenshots).
const DEBUG = GLib.getenv('YOUCAM_EXCLUSION_DEBUG') === '1';
const counts = {screen: 0, skipped: 0};

// Paints the actor only when it is going to the screen.
const HideFromCaptureEffect = GObject.registerClass(
class HideFromCaptureEffect extends Clutter.Effect {
    vfunc_paint(node, paintContext, flags) {
        const onScreen = screenFramebuffers.includes(paintContext.get_framebuffer());
        counts[onScreen ? 'screen' : 'skipped']++;
        if (onScreen)
            super.vfunc_paint(node, paintContext, flags);
    }
});

function wantsHiding(window) {
    return window.get_role() === HIDDEN_MARK || (window.get_title() ?? '').startsWith(HIDDEN_MARK);
}

export default class CaptureExclusionExtension extends Extension {
    enable() {
        global.stage.connectObject(
            'paint-view', (_stage, view) => {
                screenFramebuffers = [view.get_framebuffer(), view.get_onscreen()];
            },
            this);
        this._afterPaintView = global.stage.connect_after('paint-view', () => (screenFramebuffers = []));
        global.display.connectObject('window-created', (_display, window) => this._track(window), this);
        // "map" comes before the window's first paint; "shown" only after it.
        global.window_manager.connectObject('map', (_wm, actor) => this._update(actor.meta_window), this);
        for (const actor of global.get_window_actors())
            this._track(actor.meta_window);
        if (DEBUG) {
            Main.overview.hide();
            this._debugTimer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 1, () => {
                console.log(`youcam-exclusion: screen ${counts.screen} skipped ${counts.skipped}`);
                return GLib.SOURCE_CONTINUE;
            });
        }
    }

    disable() {
        if (this._debugTimer)
            GLib.source_remove(this._debugTimer);
        this._debugTimer = 0;
        global.stage.disconnectObject(this);
        global.stage.disconnect(this._afterPaintView);
        screenFramebuffers = [];
        global.display.disconnectObject(this);
        global.window_manager.disconnectObject(this);
        for (const actor of global.get_window_actors()) {
            actor.meta_window.disconnectObject(this);
            actor.remove_effect_by_name(EFFECT_NAME);
        }
    }

    // The actor and the title can arrive after the window is created.
    _track(window) {
        window.connectObject('notify::title', () => this._update(window),
            'shown', () => this._update(window), this);
        this._update(window);
    }

    _update(window) {
        const actor = window.get_compositor_private();
        if (!actor)
            return;
        const has = actor.get_effect(EFFECT_NAME) !== null;
        if (wantsHiding(window) && !has)
            actor.add_effect_with_name(EFFECT_NAME, new HideFromCaptureEffect());
        else if (!wantsHiding(window) && has)
            actor.remove_effect_by_name(EFFECT_NAME);
    }
}
