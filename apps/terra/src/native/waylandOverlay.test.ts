import { describe, expect, it } from 'vitest'
import overlay from '../../native/src/wayland_overlay.c?raw'

describe('Wayland stream overlay lifecycle', () => {
  it('releases exclusive keyboard focus before notifying the stream that it closed', () => {
    const hideStart = overlay.indexOf('static void hide_overlay')
    const showStart = overlay.indexOf('static void show_overlay')
    const closeStart = overlay.indexOf('static void close_overlay')
    const loadStart = overlay.indexOf('static void load_changed')
    const hideOverlay = overlay.slice(hideStart, showStart)
    const showOverlay = overlay.slice(showStart, closeStart)
    const closeOverlay = overlay.slice(closeStart, loadStart)

    expect(hideOverlay).toContain('ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE')
    expect(hideOverlay).toContain('wl_surface_set_input_region')
    expect(hideOverlay).toContain('gtk_widget_set_opacity(state->window, 0.0)')
    expect(hideOverlay).not.toContain('gtk_widget_hide')
    expect(showOverlay).toContain('ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE')
    expect(showOverlay).toContain('gtk_widget_set_opacity(state->window, 1.0)')
    expect(overlay).not.toContain('ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE')
    expect(closeOverlay.indexOf('hide_overlay(state)')).toBeLessThan(
      closeOverlay.indexOf('TERRA_OVERLAY_CLOSED'),
    )
  })
})
