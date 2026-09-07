#!/usr/bin/env python3
"""Open the running development VM's Unix VNC display without restarting it."""
from pathlib import Path
import socket
import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkVnc', '2.0')
from gi.repository import Gtk, GtkVnc

run = (Path(__file__).resolve().parent.parent / 'vista-kvm/latest').resolve()
connection = socket.socket(socket.AF_UNIX)
connection.connect(str(run / 'vnc.sock'))
window = Gtk.Window(title='Vista Aero — live VM (Ctrl+Alt releases input)')
window.set_default_size(1024, 768)
display = GtkVnc.Display()
display.set_scaling(True)
display.set_keyboard_grab(True)
display.set_pointer_grab(True)
display.connect('vnc-initialized', lambda *_: print('Vista display connected', flush=True))
display.connect('vnc-disconnected', lambda *_: Gtk.main_quit())
window.connect('destroy', lambda *_: Gtk.main_quit())
window.add(display)
window.show_all()
if not display.open_fd(connection.detach()):
    raise SystemExit('Could not open the VNC display')
window.present()
Gtk.main()
