#!/usr/bin/env python
"""
Frontend for Onkyo controller program.
"""

HOST = 'cork'
PORT = 8701

import pygtk, gtk
import gobject
import os
import socket

SUCCESS_PREFIX = "OK:"
ERROR_PREFIX = "ERROR:"
HELLO_PREFIX = "OK:onkyocontrol"

class OnkyoClient:
    """
    This class holds information and methods for connnecting to
    the Onkyo receiver daemon program.
    """

    def __init__(self):
        # set up our socket stuff
        self._iowatchevent = -1
        self._sock = None
        self._rfile = None
        self._wfile = None

        # set up our notification file descriptor
        (fd_r, fd_w) = os.pipe()
        self._piperead = fd_r
        self._pipewrite = fd_w

        self.valid_inputs = [ 'CABLE', 'TV', 'AUX', 'DVD', 'CD',
                'FM', 'AM', 'TUNER' ]
        self.status = dict()
        self.status['power'] = None
        self.status['mute'] = None
        self.status['mode'] = None
        self.status['volume'] = None
        self.status['input'] = None
        self.status['tune'] = None

    def __del__(self):
        if self._sock:
            self.disconnect()
        if self._piperead:
            os.close(self._piperead)
            os.close(self._pipewrite)

    def connect(self):
        if self._sock:
            return
        msg = "getaddrinfo returns an empty list"
        try:
            flags = socket.AI_ADDRCONFIG
        except AttributeError:
            flags = 0
        for res in socket.getaddrinfo(HOST, PORT, socket.AF_UNSPEC,
                socket.SOCK_STREAM, socket.IPPROTO_TCP, flags):
            af, socktype, proto, canonname, sa = res
            try:
                self._sock = socket.socket(af, socktype, proto)
                self._sock.connect(sa)
            except socket.error, msg:
                if self._sock:
                    self._sock.close()
                self._sock = None
                continue
            break
        if not self._sock:
            raise socket.error(msg)
        self._rfile = self._sock.makefile("rb")
        self._wfile = self._sock.makefile("wb")
        try:
            self._hello()
        except:
            self.disconnect()
            raise
        # we've verified the connection, get an initial status dump
        self.querystatus()
        # set up our select-like watcher on our input
        eventid = gobject.io_add_watch(self._rfile,
                gobject.IO_IN | gobject.IO_PRI | gobject.IO_ERR |
                gobject.IO_HUP, self._processinput)
        self._iowatchevent = eventid

    def disconnect(self):
        if self._iowatchevent > 0:
            gobject.source_remove(self._iowatchevent)
        if self._rfile:
            self._rfile.close()
            self._rfile = None
        if self._wfile:
            self._wfile.close()
            self._wfile = None
        if self._sock:
            self._sock.close()
            self._sock = None

    def reconnect(self):
        disconnect()
        connect()

    def _hello(self):
        line = self._rfile.readline()
        if not line.endswith("\n"):
            raise Exception("Connection lost on initial read")
        line = line.rstrip("\n")
        if not line.startswith(HELLO_PREFIX):
            raise Exception("Invalid hello message: '%s'" % line)

    def _writeline(self, line):
        self._wfile.write("%s\n" % line)
        self._wfile.flush()

    def _readline(self):
        line = self._rfile.readline()
        if not line.endswith("\n"):
            raise Exception("Connection lost on read")
        line = line.rstrip("\n")
        if line.startswith(ERROR_PREFIX): 
            print "Error received: %s" % line
            raise Exception("Error received: %s" % line)
            # TODO handle error
        elif line.startswith(SUCCESS_PREFIX):
            # TODO handle success
            return line

    def get_notify_fd(self):
        return self._piperead

    def _processinput(self, fd, condition):
        if condition & gobject.IO_HUP:
            # we were disconnected, attempt to reconnect
            print "Attempting to reconnect to controller socket"
            reconnect()
        elif condition & gobject.IO_ERR:
            print "Error on socket"
            return False
        else:
            line = self._readline().split(":")
            if line[1] == "power":
                if line[2] == "on":
                    self.status['power'] = True
                else:
                    self.status['power'] = False
            elif line[1] == "mute":
                if line[2] == "on":
                    self.status['mute'] = True
                else:
                    self.status['mute'] = False
            elif line[1] == "mode":
                self.status['mode'] = line[2]
            elif line[1] == "volume":
                self.status['volume'] = int(line[2])
            elif line[1] == "input":
                self.status['input'] = line[2]
            elif line[1] == "tune":
                self.status['tune'] = line[2]
            else:
                print "Unrecognized response: %s" % line

            # notify on the pipe that we processed input
            os.write(self._pipewrite, "\0")

        # return true in any case if we made it here
        return True

    def querystatus(self):
        self._writeline("status")

    def setpower(self, state):
        self.status['power'] = bool(state)
        if state == True:
            self._writeline("power on")
        elif state == False:
            self._writeline("power off")

    def setmute(self, state):
        self.status['mute'] = bool(state)
        if state == True:
            self._writeline("mute on")
        else:
            self._writeline("mute off")

    def setvolume(self, volume):
        if volume < 0 or volume > 65:
            raise Exception("Volume out of range: %d" % volume)
        self.status['volume'] = volume
        self._writeline("volume %d" % volume)

    def setinput(self, input):
        if input not in self.valid_inputs:
            raise Exception("Input not valid: %s" % input)
        self.status['input'] = input
        self._writeline("input %s" % input)

    def settune(self, freq):
        floatval = float(freq)
        if floatval < 87.4 or floatval > 108.0:
            # try AM instead
            if floatval < 530 or floatval > 1710:
              raise Exception("Frequency not valid: %f" % floatval)
            else:
                # valid AM frequency
                self._writeline("tune %d" % floatval)
        else:
            # valid FM frequency
            self._writeline("tune %f" % floatval)

class OnkyoFrontend:

    def __init__(self):
        # initialize our known status object
        self.known_status = dict()
        self.known_status['power'] = None
        self.known_status['mute'] = None
        self.known_status['mode'] = None
        self.known_status['volume'] = None
        self.known_status['input'] = None
        self.known_status['tune'] = None

        # create a new client object
        self.client = OnkyoClient()
        self.client.connect()

        # make our GTK window
        self.setup_gui()

        # kick off our update function, listening on the client notify FD
        gobject.io_add_watch(self.client.get_notify_fd(), gobject.IO_IN,
                self.update_controls)
        #gobject.timeout_add(500, self.update_controls)

    def set_combobox_text(self, combobox, text):
        model = combobox.get_model()
        iter = model.get_iter_root()
        while iter != None:
            if model.get_value(iter, 0) == text:
                combobox.set_active_iter(iter)
                break
            iter = model.iter_next(iter)

    def delete_event(self, widget, data=None):
        return False
    
    def destroy(self, widget, data=None):
        self.client.disconnect()
        gtk.main_quit()
    
    def callback_power(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['power']:
            self.client.setpower(value)

    def callback_input(self, widget, data=None):
        model = widget.get_model()
        iter = widget.get_active_iter()
        value = model.get_value(iter, 0)
        if value == self.known_status['input']:
            return
        if value == "FM Tuner":
            value = "FM"
        elif value == "AM Tuner":
            value = "AM"
        self.client.setinput(value)

    def callback_mute(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['mute']:
            self.client.setmute(value)

    def callback_volume(self, widget, data=None):
        value = widget.get_value()
        if value != self.known_status['volume']:
            self.client.setvolume(value)

    def callback_tune(self, widget, data=None):
        value = self.tuneentry.get_text()
        self.tuneentry.set_text("")
        self.client.settune(value)

    #def update_controls(self):
    def update_controls(self, fd, condition):
        # suck our input out so we don't process it again
        os.read(fd, 16)
        client_status = self.client.status
        # record our client statues in known_status so we don't do unnecessary
        # updates when we call each of the set_* methods
        statuses = [ 'power', 'mute', 'mode', 'volume', 'input', 'tune' ]
        for item in statuses:
            self.known_status[item] = client_status[item]
        # make sure to call correct method for each type of control
        # check for None value as it means we don't know the status
        if client_status['power'] != None:
            self.power.set_active(client_status['power'])
        if client_status['mute'] != None:
            self.mute.set_active(client_status['mute'])
        if client_status['mode'] != None:
            self.mode.set_text(client_status['mode'])
        if client_status['volume'] != None:
            self.volume.set_value(client_status['volume'])
        if client_status['input'] != None:
            self.set_combobox_text(self.input, client_status['input'])
        if client_status['tune'] != None:
            self.tune.set_text(client_status['tune'])

        return True

    def setup_gui(self):
        # some standard things
        self.available_inputs = [ 'Cable', 'TV', 'Aux', 'DVD', 'CD',
                'FM Tuner', 'AM Tuner' ]

        # start framing out our window
        self.window = gtk.Window(gtk.WINDOW_TOPLEVEL)
        self.window.set_title("Onkyo Receiver Controller")
        self.window.set_role("mainWindow")
        self.window.set_resizable(True)
        self.window.set_border_width(10)
        self.window.connect("delete_event", self.delete_event)
        self.window.connect("destroy", self.destroy)

        # we are going to need some boxes
        self.mainbox = gtk.HBox(False, 10)
        self.leftbox = gtk.VBox(False, 10)
        self.rightbox = gtk.VBox(False, 10)
        self.mainbox.pack_start(self.leftbox, True, False, 0)
        self.mainbox.pack_end(self.rightbox, False, False, 0)
        self.window.add(self.mainbox)

        # left box elements
        self.powerbox = gtk.HBox(False, 0)
        self.power = gtk.ToggleButton("Power")
        self.power.connect("toggled", self.callback_power)
        self.powerbox.pack_start(self.power, False, False, 0)
        self.leftbox.pack_start(self.powerbox, False, False, 0)
        self.power.show()
        self.powerbox.show()

        self.inputbox = gtk.HBox(False, 0)
        self.inputlabel = gtk.Label("Input: ")
        self.input = gtk.combo_box_new_text()
        for value in self.available_inputs:
            self.input.append_text(value)
        self.input.connect("changed", self.callback_input)
        self.inputbox.pack_start(self.inputlabel, False, False, 0)
        self.inputbox.pack_end(self.input, True, False, 0)
        self.leftbox.pack_start(self.inputbox, False, False, 0)
        self.inputlabel.show()
        self.input.show()
        self.inputbox.show()

        self.modebox = gtk.HBox(False, 0)
        self.modelabel = gtk.Label("Listening Mode: ")
        self.mode = gtk.Label()
        self.modebox.pack_start(self.modelabel, False, False, 0)
        self.modebox.pack_end(self.mode, True, True, 0)
        self.leftbox.pack_start(self.modebox, False, False, 0)
        self.modelabel.show()
        self.mode.show()
        self.modebox.show()

        self.tunebox = gtk.HBox(False, 0)
        self.tunelabel = gtk.Label("Tuned Station: ")
        self.tune = gtk.Label()
        self.tunebox.pack_start(self.tunelabel, False, False, 0)
        self.tunebox.pack_end(self.tune, True, True, 0)
        self.leftbox.pack_start(self.tunebox, False, False, 0)
        self.tunelabel.show()
        self.tune.show()
        self.tunebox.show()

        self.tuneentrybox = gtk.HBox(False, 0)
        self.tuneentrylabel = gtk.Label("Tune To: ")
        self.tuneentry = gtk.Entry(10)
        self.tuneentrybutton = gtk.Button("Go")
        self.tuneentrybutton.connect("clicked", self.callback_tune)
        self.tuneentrybox.pack_start(self.tuneentrylabel, False, False, 0)
        self.tuneentrybox.pack_end(self.tuneentrybutton, False, False, 0)
        self.tuneentrybox.pack_end(self.tuneentry, True, True, 0)
        self.leftbox.pack_start(self.tuneentrybox, False, False, 0)
        self.tuneentrylabel.show()
        self.tuneentry.show()
        self.tuneentrybutton.show()
        self.tuneentrybox.show()

        self.consolelabelbox = gtk.HBox(False, 0)
        self.consolelabel = gtk.Label("Console:")
        self.consolelabelbox.pack_start(self.consolelabel, False, False, 0)
        self.consolebox = gtk.VBox(False, 0)
        self.console = gtk.TextView()
        self.console.set_cursor_visible(False)
        self.console.set_editable(False)
        self.consolebox.pack_start(self.consolelabelbox, False, False, 0)
        self.consolebox.pack_end(self.console, True, True, 0)
        self.leftbox.pack_end(self.consolebox, True, False, 0)
        self.consolelabel.show()
        self.consolelabelbox.show()
        self.console.show()
        self.consolebox.show()

        # right box elements
        self.volumelabel = gtk.Label("Volume:")
        self.rightbox.pack_start(self.volumelabel, False, False, 0)
        self.volumelabel.show()

        self.volume = gtk.VScale()
        self.volume.set_digits(0)
        self.volume.set_range(0, 100)
        self.volume.set_inverted(True)
        self.volume.connect("value-changed", self.callback_volume)
        self.rightbox.pack_start(self.volume, True, True, 0)
        self.volume.show()

        self.mute = gtk.ToggleButton("Mute")
        self.mute.connect("toggled", self.callback_mute)
        self.rightbox.pack_start(self.mute, False, False, 0)
        self.mute.show()

        # our initial window is ready, show it
        self.leftbox.show()
        self.rightbox.show()
        self.mainbox.show()
        self.window.show()

    def main(self):
        gtk.main()

if __name__ == "__main__":
    app = OnkyoFrontend()
    app.main()

# vim: set ts=4 sw=4 et:
