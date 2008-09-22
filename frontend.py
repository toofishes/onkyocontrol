#!/usr/bin/env python
"""
Frontend for Onkyo controller program.
"""

DEBUG = True

HOST = 'cork'
PORT = 8701

import pygtk, gtk, gobject
import os, socket

HELLO_MESSAGE = "OK:onkyocontrol"
statuses = [ 'power', 'mute', 'mode', 'volume', 'input', 'tune',
        'zone2power', 'zone2mute', 'zone2volume', 'zone2input', 'zone2tune' ]

class OnkyoClientException(Exception):
    pass

class ConnectionError(OnkyoClientException):
    pass

class CommandException(OnkyoClientException):
    pass

class OnkyoClient:
    """
    This class holds information and methods for connecting to
    the Onkyo receiver daemon program.
    """

    def __init__(self):
        # set up holding spaces for our two events- a timed event if we
        # can't immediately connect, and a watch event once we are connected
        self._connectevent = -1
        self._iowatchevent = -1

        # set up our socket descriptors
        self._sock = None
        self._rfile = None
        self._wfile = None

        # set up our notification file descriptor
        (fd_r, fd_w) = os.pipe()
        self._piperead = fd_r
        self._pipewrite = fd_w

        # our status container object
        self.status = dict()
        for item in statuses:
            self.status[item] = None

        # attempt initial connection
        self.establish_connection()

    def __del__(self):
        if self._sock:
            self._disconnect()
        if self._piperead:
            os.close(self._piperead)
            os.close(self._pipewrite)

    def _connect(self):
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
            # problem opening socket, let caller know
            return False
        self._rfile = self._sock.makefile("rb")
        self._wfile = self._sock.makefile("wb")
        try:
            self._hello()
        except:
            self._disconnect()
            return False
        return True

    def _disconnect(self):
        if self._iowatchevent >= 0:
            gobject.source_remove(self._iowatchevent)
            self._iowatchevent = -1
        if self._rfile:
            self._rfile.close()
            self._rfile = None
        if self._wfile:
            self._wfile.close()
            self._wfile = None
        if self._sock:
            self._sock.close()
            self._sock = None

    def establish_connection(self, force=False):
        if self._sock:
            if force:
                self._disconnect()
            else:
                return True
        # attempt connection, we know we are disconnected at this point
        if self._connect():
            # we've verified the connection, get an initial status dump
            self.querystatus()
            # stop our timer connect event if it exists
            if self._connectevent >= 0:
                gobject.source_remove(self._connectevent)
                self._connectevent = -1
            # set up our select-like watcher on our input
            eventid = gobject.io_add_watch(self._rfile,
                    gobject.IO_IN | gobject.IO_PRI | gobject.IO_ERR |
                    gobject.IO_HUP, self._processinput)
            self._iowatchevent = eventid
            return True
        else:
            # connection failed, set up our timer connect event if it doesn't
            # already exist
            if self._connectevent < 0:
                # attempt again in 2 seconds
                eventid = gobject.timeout_add(2000, self.establish_connection)
                self._connectevent = eventid
            return True

    def _hello(self):
        line = self._rfile.readline()
        if not line.endswith("\n"):
            raise ConnectionError("Connection lost on initial read")
        line = line.rstrip("\n")
        if not line.startswith(HELLO_MESSAGE):
            raise ConnectionError("Invalid hello message: '%s'" % line)

    def _writeline(self, line):
        if DEBUG:
            print "sending line: %s" % line
        if self._wfile == None:
            self.establish_connection();
            return False
        self._wfile.write("%s\n" % line)
        self._wfile.flush()
        return True

    def _readline(self):
        line = self._rfile.readline()
        if not line.endswith("\n"):
            raise ConnectionError("Connection lost on read")
        line = line.rstrip("\n")
        if DEBUG:
            print "received line: %s" % line
        return line

    def _processinput(self, fd, condition):
        if condition & gobject.IO_HUP:
            # we were disconnected, attempt to reconnect
            print "Attempting to reconnect to controller socket"
            self.establish_connection(True)
            return False
        elif condition & gobject.IO_ERR:
            print "Error on socket"
            return False
        else:
            try:
                data = self._readline()
            except ConnectionError, e:
                # we were disconnected, attempt to reconnect
                print "Attempting to reconnect to controller socket"
                self.establish_connection(True)
                return False
            line = data.split(":")
            # first check for errors
            if line[0] == "ERROR":
                print "Error received: %s" % line
                raise OnkyoClientException("Error received: %s" % line)
            # primary zone processing
            elif line[1] == "power":
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
            # zone 2 processing
            elif line[1] == "zone2power":
                if line[2] == "on":
                    self.status['zone2power'] = True
                else:
                    self.status['zone2power'] = False
            elif line[1] == "zone2mute":
                if line[2] == "on":
                    self.status['zone2mute'] = True
                else:
                    self.status['zone2mute'] = False
            elif line[1] == "zone2mode":
                self.status['zone2mode'] = line[2]
            elif line[1] == "zone2volume":
                self.status['zone2volume'] = int(line[2])
            elif line[1] == "zone2input":
                self.status['zone2input'] = line[2]
            elif line[1] == "zone2tune":
                self.status['zone2tune'] = line[2]
            # not sure what we have if we get here
            else:
                if DEBUG:
                    print "Unrecognized response: %s" % line

            # notify the pipe that we processed input
            os.write(self._pipewrite, data)

        # return true in any case if we made it here
        return True

    def get_notify_fd(self):
        return self._piperead

    def querystatus(self):
        self._writeline("status")
        self._writeline("status zone2")

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
        try:
            intval = int(volume)
        except valueError:
            raise CommandException("Volume not an integer: %s" % volume)
        if intval < 0 or intval > 100:
            raise CommandException("Volume out of range: %d" % intval)
        self.status['volume'] = intval
        self._writeline("volume %d" % intval)

    def setinput(self, input):
        valid_inputs = [ 'CABLE', 'TV', 'AUX', 'DVD', 'CD',
                'FM', 'FM TUNER', 'AM', 'AM TUNER', 'TUNER' ]
        if input.upper() not in valid_inputs:
            raise CommandException("Input not valid: %s" % input)
        self.status['input'] = input
        self._writeline("input %s" % input)

    def settune(self, freq):
        try:
            floatval = float(freq)
        except ValueError:
            raise CommandException("Frequency not valid: %s" % freq)
        # attempt to validate the frequency
        if floatval < 87.4 or floatval > 108.0:
            # try AM instead
            if floatval < 530 or floatval > 1710:
                # we failed both validity tests
                raise CommandException("Frequency not valid: %s" % freq)
            else:
                # valid AM frequency
                self._writeline("tune %d" % floatval)
        else:
            # valid FM frequency
            self._writeline("tune %.1f" % floatval)

    def setzone2power(self, state):
        self.status['zone2power'] = bool(state)
        if state == True:
            self._writeline("z2power on")
        elif state == False:
            self._writeline("z2power off")

    def setzone2mute(self, state):
        self.status['zone2mute'] = bool(state)
        if state == True:
            self._writeline("z2mute on")
        else:
            self._writeline("z2mute off")

    def setzone2volume(self, volume):
        try:
            intval = int(volume)
        except valueError:
            raise CommandException("Volume not an integer: %s" % volume)
        if intval < 0 or intval > 100:
            raise CommandException("Volume out of range: %d" % intval)
        self.status['zone2volume'] = intval
        self._writeline("z2volume %d" % intval)

    def setzone2input(self, input):
        valid_inputs = [ 'CABLE', 'TV', 'AUX', 'DVD', 'CD',
                'FM', 'FM TUNER', 'AM', 'AM TUNER', 'TUNER',
                'SOURCE', 'OFF' ]
        if input.upper() not in valid_inputs:
            raise CommandException("Input not valid: %s" % input)
        self.status['zone2input'] = input
        self._writeline("z2input %s" % input)


class OnkyoFrontend:

    def __init__(self):
        # initialize our known status object
        self.known_status = dict()
        for item in statuses:
            self.known_status[item] = None

        # create a new client object
        self.client = OnkyoClient()

        # make our GTK window
        self.setup_gui()

        # kick off our update function, listening on the client notify FD
        gobject.io_add_watch(self.client.get_notify_fd(), gobject.IO_IN,
                self.update_controls)

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
        gtk.main_quit()

    def errorbox(self, message):
        def response_handler(dialog, response_id):
            dialog.destroy()
        dialog = gtk.MessageDialog(self.window,
                gtk.DIALOG_DESTROY_WITH_PARENT,
                gtk.MESSAGE_ERROR,
                gtk.BUTTONS_OK,
                message)
        dialog.connect("response", response_handler)
        dialog.show()

    def callback_power(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['power']:
            if value == False:
                # prompt to make sure we actually want to power down
                def response_handler(dialog, response_id):
                    if response_id == gtk.RESPONSE_YES:
                        self.client.setpower(False)
                    else:
                        # we need to toggle the button back, no was pressed
                        widget.set_active(True)
                    dialog.destroy()
                dialog = gtk.MessageDialog(self.window,
                        gtk.DIALOG_DESTROY_WITH_PARENT,
                        gtk.MESSAGE_QUESTION,
                        gtk.BUTTONS_YES_NO,
                        "Are you sure you wish to power down the receiver?")
                dialog.connect("response", response_handler)
                dialog.show()
            else:
                # just turn it on without confirmation if we were in off state
                self.client.setpower(True)

    def callback_zone2power(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['zone2power']:
            self.client.setzone2power(value)

    def callback_input(self, widget, data=None):
        model = widget.get_model()
        iter = widget.get_active_iter()
        value = model.get_value(iter, 0)
        if value == self.known_status['input']:
            return
        self.client.setinput(value)

    def callback_zone2input(self, widget, data=None):
        model = widget.get_model()
        iter = widget.get_active_iter()
        value = model.get_value(iter, 0)
        if value == self.known_status['zone2input']:
            return
        self.client.setzone2input(value)

    def callback_mute(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['mute']:
            self.client.setmute(value)

    def callback_zone2mute(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['zone2mute']:
            self.client.setzone2mute(value)

    def callback_volume(self, widget, data=None):
        value = int(widget.get_value())
        if value != self.known_status['volume']:
            self.client.setvolume(value)

    def callback_zone2volume(self, widget, data=None):
        value = int(widget.get_value())
        if value != self.known_status['zone2volume']:
            self.client.setzone2volume(value)

    def callback_tune(self, widget, data=None):
        value = self.tuneentry.get_text()
        self.tuneentry.set_text("")
        try:
            self.client.settune(value)
        except CommandException, e:
            self.errorbox(e.args[0])

    def update_controls(self, fd, condition):
        # suck our input, put it in the console
        data = os.read(fd, 256)
        buf = self.console.get_buffer()
        buf.insert(buf.get_end_iter(), "%s\n" % data)
        self.console.scroll_to_iter(buf.get_end_iter(), 0)
        # convenience variable
        client_status = self.client.status
        # record our client statues in known_status so we don't do unnecessary
        # updates when we call each of the set_* methods
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

        if client_status['zone2power'] != None:
            self.zone2power.set_active(client_status['zone2power'])
        if client_status['zone2mute'] != None:
            self.zone2mute.set_active(client_status['zone2mute'])
        # no zone2 mode
        if client_status['zone2volume'] != None:
            self.zone2volume.set_value(client_status['zone2volume'])
        if client_status['zone2input'] != None:
            self.set_combobox_text(self.zone2input, client_status['zone2input'])
        if client_status['zone2tune'] != None:
            self.zone2tune.set_text(client_status['zone2tune'])

        return True

    def setup_gui(self):
        # some standard things
        self.available_inputs = [ 'Cable', 'TV', 'Aux', 'DVD', 'CD',
                'FM Tuner', 'AM Tuner' ]
        self.zone2_available_inputs = [ 'Cable', 'TV', 'Aux', 'DVD', 'CD',
                'FM Tuner', 'AM Tuner', 'Source', 'Off' ]

        # start framing out our window
        self.window = gtk.Window(gtk.WINDOW_TOPLEVEL)
        self.window.set_title("Onkyo Receiver Controller")
        self.window.set_role("mainWindow")
        self.window.set_resizable(True)
        self.window.set_border_width(10)
        self.window.connect("delete_event", self.delete_event)
        self.window.connect("destroy", self.destroy)

        # we are going to need some boxes
        self.mainbox = gtk.VBox(False, 10)
        self.upperbox = gtk.HBox(False, 10)
        self.secondarybox = gtk.VBox(False, 10)
        self.primarybox = gtk.VBox(False, 10)
        self.zone2secondarybox = gtk.VBox(False, 10)
        self.zone2primarybox = gtk.VBox(False, 10)

        self.upperbox.pack_start(self.secondarybox, True, False, 0)
        self.upperbox.pack_start(self.primarybox, False, False, 0)
        self.upperbox.pack_end(self.zone2secondarybox, True, False, 0)
        self.upperbox.pack_end(self.zone2primarybox, False, False, 0)
        self.mainbox.pack_start(self.upperbox, True, True, 0)
        self.window.add(self.mainbox)

        # secondary box (detailed control) elements
        self.inputbox = gtk.HBox(False, 0)
        self.inputlabel = gtk.Label("Input: ")
        self.input = gtk.combo_box_new_text()
        for value in self.available_inputs:
            self.input.append_text(value)
        self.input.connect("changed", self.callback_input)
        self.inputbox.pack_start(self.inputlabel, False, False, 0)
        self.inputbox.pack_end(self.input, True, False, 0)
        self.secondarybox.pack_start(self.inputbox, False, False, 0)
        self.inputlabel.show()
        self.input.show()
        self.inputbox.show()

        self.modebox = gtk.HBox(False, 0)
        self.modelabel = gtk.Label("Listening Mode: ")
        self.mode = gtk.Label("Unknown")
        self.modebox.pack_start(self.modelabel, False, False, 0)
        self.modebox.pack_end(self.mode, True, True, 0)
        self.secondarybox.pack_start(self.modebox, False, False, 0)
        self.modelabel.show()
        self.mode.show()
        self.modebox.show()

        self.tunebox = gtk.HBox(False, 0)
        self.tunelabel = gtk.Label("Tuned Station: ")
        self.tune = gtk.Label("Unknown")
        self.tunebox.pack_start(self.tunelabel, False, False, 0)
        self.tunebox.pack_end(self.tune, True, True, 0)
        self.secondarybox.pack_start(self.tunebox, False, False, 0)
        self.tunelabel.show()
        self.tune.show()
        self.tunebox.show()

        self.tuneentrybox = gtk.HBox(False, 0)
        self.tuneentrylabel = gtk.Label("Tune To: ")
        self.tuneentry = gtk.Entry(10)
        self.tuneentry.connect("activate", self.callback_tune)
        self.tuneentrybutton = gtk.Button("Go")
        self.tuneentrybutton.connect("clicked", self.callback_tune)
        self.tuneentrybox.pack_start(self.tuneentrylabel, False, False, 0)
        self.tuneentrybox.pack_end(self.tuneentrybutton, False, False, 0)
        self.tuneentrybox.pack_end(self.tuneentry, True, True, 0)
        self.secondarybox.pack_start(self.tuneentrybox, False, False, 0)
        self.tuneentrylabel.show()
        self.tuneentry.show()
        self.tuneentrybutton.show()
        self.tuneentrybox.show()

        # primary control box elements
        self.power = gtk.ToggleButton("Power")
        self.power.connect("toggled", self.callback_power)
        self.primarybox.pack_start(self.power, False, False, 0)
        self.power.show()

        self.volumelabel = gtk.Label("Volume:")
        self.primarybox.pack_start(self.volumelabel, False, False, 0)
        self.volumelabel.show()

        self.volume = gtk.VScale()
        self.volume.set_digits(0)
        self.volume.set_range(0, 100)
        self.volume.set_inverted(True)
        # don't update volume value immediately, wait for settling
        self.volume.set_update_policy(gtk.UPDATE_DELAYED)
        self.volume.set_increments(1, 5)
        # make sure this widget is large enough to be usable
        self.volume.set_size_request(-1, 125)
        self.volume.connect("value-changed", self.callback_volume)
        self.primarybox.pack_start(self.volume, True, True, 0)
        self.volume.show()

        self.mute = gtk.ToggleButton("Mute")
        self.mute.connect("toggled", self.callback_mute)
        self.primarybox.pack_start(self.mute, False, False, 0)
        self.mute.show()

        # zone 2 secondary box (detailed control) elements
        self.zone2inputbox = gtk.HBox(False, 0)
        self.zone2inputlabel = gtk.Label("Z2 Input: ")
        self.zone2input = gtk.combo_box_new_text()
        for value in self.zone2_available_inputs:
            self.zone2input.append_text(value)
        self.zone2input.connect("changed", self.callback_zone2input)
        self.zone2inputbox.pack_start(self.zone2inputlabel, False, False, 0)
        self.zone2inputbox.pack_end(self.zone2input, True, False, 0)
        self.zone2secondarybox.pack_start(self.zone2inputbox, False, False, 0)
        self.zone2inputlabel.show()
        self.zone2input.show()
        self.zone2inputbox.show()

        self.zone2modebox = gtk.HBox(False, 0)
        self.zone2modelabel = gtk.Label("Z2 Listening Mode: ")
        self.zone2mode = gtk.Label("Stereo")
        self.zone2modebox.pack_start(self.zone2modelabel, False, False, 0)
        self.zone2modebox.pack_end(self.zone2mode, True, True, 0)
        self.zone2secondarybox.pack_start(self.zone2modebox, False, False, 0)
        self.zone2modelabel.show()
        self.zone2mode.show()
        self.zone2modebox.show()

        self.zone2tunebox = gtk.HBox(False, 0)
        self.zone2tunelabel = gtk.Label("Z2 Tuned Station: ")
        self.zone2tune = gtk.Label("Unknown")
        self.zone2tunebox.pack_start(self.zone2tunelabel, False, False, 0)
        self.zone2tunebox.pack_end(self.zone2tune, True, True, 0)
        self.zone2secondarybox.pack_start(self.zone2tunebox, False, False, 0)
        self.zone2tunelabel.show()
        self.zone2tune.show()
        self.zone2tunebox.show()

        self.zone2tuneentrybox = gtk.HBox(False, 0)
        self.zone2tuneentrylabel = gtk.Label("Tune To: ")
        self.zone2tuneentry = gtk.Entry(10)
        #self.zone2tuneentry.connect("activate", self.callback_zone2tune)
        self.zone2tuneentrybutton = gtk.Button("Go")
        #self.zone2tuneentrybutton.connect("clicked", self.callback_zone2tune)
        self.zone2tuneentrybox.pack_start(self.zone2tuneentrylabel,
                False, False, 0)
        self.zone2tuneentrybox.pack_end(self.zone2tuneentrybutton,
                False, False, 0)
        self.zone2tuneentrybox.pack_end(self.zone2tuneentry,
                True, True, 0)
        self.zone2secondarybox.pack_start(self.zone2tuneentrybox,
                False, False, 0)
        self.zone2tuneentrylabel.show()
        self.zone2tuneentry.show()
        self.zone2tuneentrybutton.show()
        self.zone2tuneentrybox.show()

        # zone 2 primary control elements
        self.zone2power = gtk.ToggleButton("Z2 Power")
        self.zone2power.connect("toggled", self.callback_zone2power)
        self.zone2primarybox.pack_start(self.zone2power, False, False, 0)
        self.zone2power.show()

        self.zone2volumelabel = gtk.Label("Z2 Volume:")
        self.zone2primarybox.pack_start(self.zone2volumelabel, False, False, 0)
        self.zone2volumelabel.show()

        self.zone2volume = gtk.VScale()
        self.zone2volume.set_digits(0)
        self.zone2volume.set_range(0, 100)
        self.zone2volume.set_inverted(True)
        # don't update volume value immediately, wait for settling
        self.zone2volume.set_update_policy(gtk.UPDATE_DELAYED)
        self.zone2volume.set_increments(1, 5)
        # make sure this widget is large enough to be usable
        self.zone2volume.set_size_request(-1, 125)
        self.zone2volume.connect("value-changed", self.callback_zone2volume)
        self.zone2primarybox.pack_start(self.zone2volume, True, True, 0)
        self.zone2volume.show()

        self.zone2mute = gtk.ToggleButton("Z2 Mute")
        self.zone2mute.connect("toggled", self.callback_zone2mute)
        self.zone2primarybox.pack_start(self.zone2mute, False, False, 0)
        self.zone2mute.show()

        # add an output/debug console below everything else
        self.consolelabelbox = gtk.HBox(False, 0)
        self.consolelabel = gtk.Label("Console:")
        self.consolelabelbox.pack_start(self.consolelabel, False, False, 0)
        self.consolebox = gtk.VBox(False, 0)
        self.console = gtk.TextView()
        self.console.set_cursor_visible(False)
        self.console.set_editable(False)
        self.consolescroll = gtk.ScrolledWindow()
        self.consolescroll.set_policy(gtk.POLICY_AUTOMATIC,
                gtk.POLICY_AUTOMATIC)
        self.consolescroll.add(self.console)
        self.consolebox.pack_start(self.consolelabelbox, False, False, 0)
        self.consolebox.pack_end(self.consolescroll, True, True, 0)
        self.mainbox.pack_end(self.consolebox, False, False, 0)
        self.consolelabel.show()
        self.consolelabelbox.show()
        self.console.show()
        self.consolescroll.show()
        self.consolebox.show()

        # our initial window is ready, show it
        self.secondarybox.show()
        self.primarybox.show()
        self.zone2primarybox.show()
        self.zone2secondarybox.show()
        self.upperbox.show()
        self.mainbox.show()
        self.window.show()

    def main(self):
        gtk.main()

if __name__ == "__main__":
    app = OnkyoFrontend()
    app.main()

# vim: set ts=4 sw=4 et: