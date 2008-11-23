#!/usr/bin/env python
"""
Frontend for Onkyo controller program.
"""

DEBUG = False

HOST = 'dublin'
PORT = 8701

import pygtk, gtk, gobject
import os, socket

HELLO_MESSAGE = "OK:onkyocontrol"
STATUSES = [ 'power', 'mute', 'mode', 'volume', 'input', 'tune', 'sleep',
        'zone2power', 'zone2mute', 'zone2volume', 'zone2input', 'zone2tune' ]

def verify_frequency(freq):
    """
    Verify that a frequency value given by the user can be mapped to an FM
    or AM frequency. If it is a valid FM frequency, return the value as a
    float; if it is a valid AM frequency return it as an int. If the frequency
    is not valid, raise a CommandException.
    """
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
            return "AM", int(floatval)
    else:
        # valid FM frequency
        return "FM", floatval

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
        for item in STATUSES:
            self.status[item] = None
        self.status['epoch'] = 0

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
        except ConnectionError:
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
            # we've verified the connection, get powered on status
            self.querypower()
            # allow the frontend to see that we may have stale data
            self.status['epoch'] = self.status['epoch'] + 1
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
            self.establish_connection()
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
                #raise OnkyoClientException("Error received: %s" % line)
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
            # For everything else, we use the literal string returned after
            # the second colon. Ensure we don't lose any actual colons by
            # rejoining after the second colon.
            elif line[1] == "mode":
                self.status['mode'] = ":".join(line[2:])
            elif line[1] == "volume":
                self.status['volume'] = int(line[2])
            elif line[1] == "input":
                self.status['input'] = ":".join(line[2:])
            elif line[1] == "tune":
                self.status['tune'] = ":".join(line[2:])
            elif line[1] == "sleep":
                self.status['sleep'] = int(line[2])
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
                print "Unrecognized response: %s" % line

            # notify the pipe that we processed input
            os.write(self._pipewrite, data)

        # return true in any case if we made it here
        return True

    def get_notify_fd(self):
        return self._piperead

    def querystatus(self):
        self._writeline("status")

    def queryzone2status(self):
        self._writeline("status zone2")

    def querysleep(self):
        self._writeline("sleep")

    def querypower(self):
        self._writeline("power")
        self._writeline("z2power")

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
        except ValueError:
            raise CommandException("Volume not an integer: %s" % volume)
        if intval < 0 or intval > 100:
            raise CommandException("Volume out of range: %d" % intval)
        self.status['volume'] = intval
        self._writeline("volume %d" % intval)

    def setinput(self, inp):
        valid_inputs = [ 'dvr', 'vcr', 'cable', 'sat', 'tv', 'aux', 'dvd',
                'tape', 'phono', 'cd', 'fm', 'fm tuner', 'am', 'am tuner',
                'tuner', 'multich', 'xm', 'sirius' ]
        if inp.lower() not in valid_inputs:
            raise CommandException("Input not valid: %s" % inp)
        self.status['input'] = inp
        self._writeline("input %s" % inp)

    def setmode(self, mode):
        valid_modes = [ 'stereo', 'direct', 'acstereo', 'fullmono',
                'pure', 'straight',
                'thx', 'pliimovie', 'pliimusic', 'pliigame',
                'neo6cinema', 'neo6music', 'pliithx', 'neo6thx',
                'neuralthx' ]
        if mode.lower() not in valid_modes:
            raise CommandException("Listening mode not valid: %s" % mode)
        self.status['mode'] = mode
        self._writeline("mode %s" % mode)

    def settune(self, freq):
        # this will throw an exception if freq was invalid
        value = verify_frequency(freq)
        if value[0] == "AM":
            self._writeline("tune %d" % value[1])
        else:
            self._writeline("tune %.1f" % value[1])

    def setsleep(self, mins):
        try:
            intval = int(mins)
        except ValueError:
            raise CommandException("Sleep time not an integer: %s" % mins)
        if intval < 0 or intval > 90:
            raise CommandException("Sleep time out of range: %d" % intval)
        self.status['sleep'] = intval
        self._writeline("sleep %d" % intval)

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
        except ValueError:
            raise CommandException("Volume not an integer: %s" % volume)
        if intval < 0 or intval > 100:
            raise CommandException("Volume out of range: %d" % intval)
        self.status['zone2volume'] = intval
        self._writeline("z2volume %d" % intval)

    def setzone2input(self, inp):
        valid_inputs = [ 'dvr', 'vcr', 'cable', 'sat', 'tv', 'aux', 'dvd',
                'tape', 'phono', 'cd', 'fm', 'fm tuner', 'am', 'am tuner',
                'tuner', 'multich', 'xm', 'sirius',
                'source', 'off' ]
        if inp.lower() not in valid_inputs:
            raise CommandException("Input not valid: %s" % inp)
        self.status['zone2input'] = inp
        self._writeline("z2input %s" % inp)

    def setzone2tune(self, freq):
        # this will throw an exception if freq was invalid
        value = verify_frequency(freq)
        if value[0] == "AM":
            self._writeline("z2tune %d" % value[1])
        else:
            self._writeline("z2tune %.1f" % value[1])


class OnkyoFrontend:

    def __init__(self):
        # initialize our known status object
        self.known_status = dict()
        for item in STATUSES:
            self.known_status[item] = None
        self.known_status['epoch'] = 0

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
                        self.set_main_sensitive(False)
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
                self.set_main_sensitive(True)

    def callback_zone2power(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['zone2power']:
            self.client.setzone2power(value)
            self.set_zone2_sensitive(value)

    def callback_input(self, widget, data=None):
        model = widget.get_model()
        iter = widget.get_active_iter()
        key = model.get_value(iter, 0)
        if key == self.known_status['input']:
            return
        value = self.available_inputs_dict[key]
        self.client.setinput(value)

    def callback_zone2input(self, widget, data=None):
        model = widget.get_model()
        iter = widget.get_active_iter()
        key = model.get_value(iter, 0)
        if key == self.known_status['zone2input']:
            return
        value = self.zone2_available_inputs_dict[key]
        self.client.setzone2input(value)

    def callback_mode(self, widget, data=None):
        model = widget.get_model()
        iter = widget.get_active_iter()
        key = model.get_value(iter, 0)
        if key == self.known_status['mode']:
            return
        value = self.available_modes_dict[key]
        self.client.setmode(value)


    def callback_mute(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['mute']:
            self.client.setmute(value)

    def callback_zone2mute(self, widget, data=None):
        value = widget.get_active()
        if value != self.known_status['zone2mute']:
            self.client.setzone2mute(value)

    def callback_format_volume(self, widget, value, data=None):
        # value comes in between 0 and 100. By subtracting 82, we get our
        # dB reading, which is the same as shown on the receiver.
        intval = int(value) - 82
        if intval <= -82:
            # -(infinity) dB
            return u"-\u221E dB"
        return "%+d dB" % intval

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

    def callback_sleep(self, widget, data=None):
        value = self.sleepentry.get_text()
        self.sleepentry.set_text("")
        try:
            self.client.setsleep(value)
        except CommandException, e:
            self.errorbox(e.args[0])

    def callback_zone2tune(self, widget, data=None):
        value = self.zone2tuneentry.get_text()
        self.zone2tuneentry.set_text("")
        try:
            self.client.setzone2tune(value)
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
        status_updated = dict()
        # record our client statues in known_status so we don't do unnecessary
        # updates when we call each of the set_* methods
        for item in STATUSES:
            if self.known_status[item] != client_status[item]:
                self.known_status[item] = client_status[item]
                status_updated[item] = True
            else:
                status_updated[item] = False
        new_epoch = self.known_status['epoch'] != client_status['epoch']
        if new_epoch:
            self.known_status['epoch'] = client_status['epoch']


        # make sure to call correct method for each type of control
        # check for None value as it means we don't know the status
        if client_status['power'] != None and status_updated['power'] == True:
            self.power.set_active(client_status['power'])
            self.set_main_sensitive(client_status['power'])
            # query for a status update if the receiver power switched on
            if client_status['power'] == True:
                self.client.querystatus()
                self.client.querysleep()
        # also query for a status update if power is on and new epoch
        elif client_status['power'] == True and new_epoch:
            self.client.querystatus()
            self.client.querysleep()
        if client_status['mute'] != None and status_updated['mute'] == True:
            self.mute.set_active(client_status['mute'])
        if client_status['mode'] != None and status_updated['mode'] == True:
            self.set_combobox_text(self.mode, client_status['mode'])
        if client_status['volume'] != None and status_updated['volume'] == True:
            self.volume.set_value(client_status['volume'])
        if client_status['input'] != None and status_updated['input'] == True:
            self.set_combobox_text(self.input, client_status['input'])
        if client_status['tune'] != None and status_updated['tune'] == True:
            self.tune.set_text(client_status['tune'])
        if client_status['sleep'] != None and status_updated['sleep'] == True:
            if client_status['sleep'] == 0:
                self.sleep.set_text("Off")
            else:
                self.sleep.set_text("%d min" % client_status['sleep'])

        if client_status['zone2power'] != None and \
                status_updated['zone2power'] == True:
            self.zone2power.set_active(client_status['zone2power'])
            self.set_zone2_sensitive(client_status['zone2power'])
            if client_status['zone2power'] == True:
                self.client.queryzone2status()
                self.client.querysleep()
        # also query for a status update if power is on and new epoch
        elif client_status['zone2power'] == True and new_epoch:
            self.client.queryzone2status()
            self.client.querysleep()
        if client_status['zone2mute'] != None and \
                status_updated['zone2mute'] == True:
            self.zone2mute.set_active(client_status['zone2mute'])
        # no zone2 mode
        if client_status['zone2volume'] != None and \
                status_updated['zone2volume'] == True:
            self.zone2volume.set_value(client_status['zone2volume'])
        if client_status['zone2input'] != None and \
                status_updated['zone2input'] == True:
            self.set_combobox_text(self.zone2input, client_status['zone2input'])
        if client_status['zone2tune'] != None and \
                status_updated['zone2tune'] == True:
            self.zone2tune.set_text(client_status['zone2tune'])

        return True

    def set_main_sensitive(self, sensitive):
        self.volume.set_sensitive(sensitive)
        self.mute.set_sensitive(sensitive)
        self.input.set_sensitive(sensitive)
        self.mode.set_sensitive(sensitive)
        self.tune.set_sensitive(sensitive)
        self.tuneentry.set_sensitive(sensitive)
        self.tuneentrybutton.set_sensitive(sensitive)

    def set_zone2_sensitive(self, sensitive):
        self.zone2volume.set_sensitive(sensitive)
        self.zone2mute.set_sensitive(sensitive)
        self.zone2input.set_sensitive(sensitive)
        self.zone2mode.set_sensitive(sensitive)
        self.zone2tune.set_sensitive(sensitive)
        self.zone2tuneentry.set_sensitive(sensitive)
        self.zone2tuneentrybutton.set_sensitive(sensitive)

    def setup_gui(self):
        # some standard things
        self.available_inputs = [
                ('DVR', 'dvr'),
                ('Cable', 'cable'),
                ('TV', 'tv'),
                ('Aux', 'aux'),
                ('DVD', 'dvd'),
                ('Tape', 'tape'),
                ('Phono', 'phono'),
                ('CD', 'cd'),
                ('FM Tuner', 'fm'),
                ('AM Tuner', 'am'),
                ('Multichannel', 'multich'),
                ('XM Radio', 'xm'),
                ('Sirius Radio', 'sirius'),
        ]
        self.available_inputs_dict = dict(self.available_inputs)

        # make a copy for zone 2, adding one element
        self.zone2_available_inputs = self.available_inputs[:]
        self.zone2_available_inputs.append( ('Source', 'source') )
        self.zone2_available_inputs_dict = dict(self.zone2_available_inputs)

        self.available_modes = [
                ('Stereo', 'stereo'),
                ('Direct', 'direct'),
                ('All Channel Stereo', 'acstereo'),
                ('Full Mono', 'fullmono'),
                ('Pure Audio', 'pure'),
                ('Straight Decode', 'straight'),
                ('THX Cinema', 'thx'),
                ('Pro Logic IIx Movie', 'pliimovie'),
                ('Pro Logic IIx Music', 'pliimusic'),
                ('Pro Logic IIx Game', 'pliigame'),
                ('Neo:6 Cinema', 'neo6cinema'),
                ('Neo:6 Music', 'neo6music'),
                ('PLIIx THX Cinema', 'pliithx'),
                ('Neo:6 THX Cinema', 'neo6thx'),
                ('Neural THX', 'neuralthx'),
        ]
        self.available_modes_dict = dict(self.available_modes)

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
        self.mainzonelabel = gtk.Label()
        self.mainzonelabel.set_markup("<b>Main Zone</b>")
        self.mainzonelabel.set_justify(gtk.JUSTIFY_CENTER)
        self.secondarybox.pack_start(self.mainzonelabel, False, False, 0)
        self.mainzonelabel.show()

        self.inputbox = gtk.HBox(False, 0)
        self.inputlabel = gtk.Label("Input: ")
        self.input = gtk.combo_box_new_text()
        for value in self.available_inputs:
            self.input.append_text(value[0])
        self.input.connect("changed", self.callback_input)
        self.inputbox.pack_start(self.inputlabel, False, False, 0)
        self.inputbox.pack_end(self.input, False, False, 0)
        self.secondarybox.pack_start(self.inputbox, False, False, 0)
        self.inputlabel.show()
        self.input.show()
        self.inputbox.show()

        self.modebox = gtk.HBox(False, 0)
        self.modelabel = gtk.Label("Listening Mode: ")
        self.mode = gtk.combo_box_new_text()
        for modeval in self.available_modes:
            self.mode.append_text(modeval[0])
        self.mode.connect("changed", self.callback_mode)
        self.modebox.pack_start(self.modelabel, False, False, 0)
        self.modebox.pack_end(self.mode, False, False, 0)
        self.secondarybox.pack_start(self.modebox, False, False, 0)
        self.modelabel.show()
        self.mode.show()
        self.modebox.show()

        self.tunebox = gtk.HBox(False, 0)
        self.tunelabel = gtk.Label("Tuned Station: ")
        self.tune = gtk.Label("Unknown")
        self.tune.set_justify(gtk.JUSTIFY_RIGHT)
        self.tunebox.pack_start(self.tunelabel, False, False, 0)
        self.tunebox.pack_end(self.tune, False, False, 0)
        self.secondarybox.pack_start(self.tunebox, False, False, 0)
        self.tunelabel.show()
        self.tune.show()
        self.tunebox.show()

        self.tuneentrybox = gtk.HBox(False, 0)
        self.tuneentrylabel = gtk.Label("Tune To: ")
        self.tuneentry = gtk.Entry(6)
        self.tuneentry.set_width_chars(15)
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

        self.sleepentrybox = gtk.HBox(False, 0)
        self.sleepentrylabel = gtk.Label("Sleep Timer: ")
        self.sleep = gtk.Label("Off")
        self.sleepentry = gtk.Entry(2)
        self.sleepentry.set_width_chars(6)
        self.sleepentry.connect("activate", self.callback_sleep)
        self.sleepentrybutton = gtk.Button("Set")
        self.sleepentrybutton.connect("clicked", self.callback_sleep)
        self.sleepentrybox.pack_start(self.sleepentrylabel, False, False, 0)
        self.sleepentrybox.pack_start(self.sleep, False, False, 0)
        self.sleepentrybox.pack_end(self.sleepentrybutton, False, False, 0)
        self.sleepentrybox.pack_end(self.sleepentry, False, True, 0)
        self.secondarybox.pack_start(self.sleepentrybox, False, False, 0)
        self.sleepentrylabel.show()
        self.sleep.show()
        self.sleepentry.show()
        self.sleepentrybutton.show()
        self.sleepentrybox.show()

        # primary control box elements
        self.power = gtk.ToggleButton("Power")
        self.power.connect("toggled", self.callback_power)
        self.primarybox.pack_start(self.power, False, False, 0)
        self.power.show()

        self.volumelabel = gtk.Label("Volume")
        self.volumelabel.set_justify(gtk.JUSTIFY_CENTER)
        self.primarybox.pack_start(self.volumelabel, False, False, 0)
        self.volumelabel.show()

        self.volume = gtk.VScale()
        self.volume.set_range(0, 100)
        self.volume.set_inverted(True)
        # don't update volume value immediately, wait for settling
        self.volume.set_update_policy(gtk.UPDATE_DELAYED)
        self.volume.set_increments(1, 5)
        # make sure this widget is large enough to be usable
        self.volume.set_size_request(-1, 125)
        self.volume.connect("value-changed", self.callback_volume)
        self.volume.connect("format-value", self.callback_format_volume)
        self.primarybox.pack_start(self.volume, True, True, 0)
        self.volume.show()

        self.mute = gtk.ToggleButton("Mute")
        self.mute.connect("toggled", self.callback_mute)
        self.primarybox.pack_start(self.mute, False, False, 0)
        self.mute.show()

        # zone 2 secondary box (detailed control) elements
        self.zonetwolabel = gtk.Label()
        self.zonetwolabel.set_markup("<b>Zone Two</b>")
        self.zonetwolabel.set_justify(gtk.JUSTIFY_CENTER)
        self.zone2secondarybox.pack_start(self.zonetwolabel, False, False, 0)
        self.zonetwolabel.show()

        self.zone2inputbox = gtk.HBox(False, 0)
        self.zone2inputlabel = gtk.Label("Input: ")
        self.zone2input = gtk.combo_box_new_text()
        for value in self.zone2_available_inputs:
            self.zone2input.append_text(value[0])
        self.zone2input.connect("changed", self.callback_zone2input)
        self.zone2inputbox.pack_start(self.zone2inputlabel, False, False, 0)
        self.zone2inputbox.pack_end(self.zone2input, False, False, 0)
        self.zone2secondarybox.pack_start(self.zone2inputbox, False, False, 0)
        self.zone2inputlabel.show()
        self.zone2input.show()
        self.zone2inputbox.show()

        self.zone2modebox = gtk.HBox(False, 0)
        self.zone2modelabel = gtk.Label("Listening Mode: ")
        self.zone2mode = gtk.Label("Stereo")
        self.zone2mode.set_justify(gtk.JUSTIFY_RIGHT)
        self.zone2modebox.pack_start(self.zone2modelabel, False, False, 0)
        self.zone2modebox.pack_end(self.zone2mode, False, False, 0)
        self.zone2secondarybox.pack_start(self.zone2modebox, False, False, 0)
        self.zone2modelabel.show()
        self.zone2mode.show()
        self.zone2modebox.show()

        self.zone2tunebox = gtk.HBox(False, 0)
        self.zone2tunelabel = gtk.Label("Tuned Station: ")
        self.zone2tune = gtk.Label("Unknown")
        self.zone2tune.set_justify(gtk.JUSTIFY_RIGHT)
        self.zone2tunebox.pack_start(self.zone2tunelabel, False, False, 0)
        self.zone2tunebox.pack_end(self.zone2tune, False, False, 0)
        self.zone2secondarybox.pack_start(self.zone2tunebox, False, False, 0)
        self.zone2tunelabel.show()
        self.zone2tune.show()
        self.zone2tunebox.show()

        self.zone2tuneentrybox = gtk.HBox(False, 0)
        self.zone2tuneentrylabel = gtk.Label("Tune To: ")
        self.zone2tuneentry = gtk.Entry(6)
        self.zone2tuneentry.set_width_chars(15)
        self.zone2tuneentry.connect("activate", self.callback_zone2tune)
        self.zone2tuneentrybutton = gtk.Button("Go")
        self.zone2tuneentrybutton.connect("clicked", self.callback_zone2tune)
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

        self.zone2volumelabel = gtk.Label("Z2 Volume")
        self.zone2volumelabel.set_justify(gtk.JUSTIFY_CENTER)
        self.zone2primarybox.pack_start(self.zone2volumelabel, False, False, 0)
        self.zone2volumelabel.show()

        self.zone2volume = gtk.VScale()
        self.zone2volume.set_range(0, 100)
        self.zone2volume.set_inverted(True)
        # don't update volume value immediately, wait for settling
        self.zone2volume.set_update_policy(gtk.UPDATE_DELAYED)
        self.zone2volume.set_increments(1, 5)
        # make sure this widget is large enough to be usable
        self.zone2volume.set_size_request(-1, 125)
        self.zone2volume.connect("value-changed", self.callback_zone2volume)
        self.zone2volume.connect("format-value", self.callback_format_volume)
        self.zone2primarybox.pack_start(self.zone2volume, True, True, 0)
        self.zone2volume.show()

        self.zone2mute = gtk.ToggleButton("Z2 Mute")
        self.zone2mute.connect("toggled", self.callback_zone2mute)
        self.zone2primarybox.pack_start(self.zone2mute, False, False, 0)
        self.zone2mute.show()

        # add an output/debug console below everything else
        self.console = gtk.TextView()
        self.console.set_cursor_visible(False)
        self.console.set_editable(False)
        self.consolescroll = gtk.ScrolledWindow()
        self.consolescroll.set_policy(gtk.POLICY_AUTOMATIC,
                gtk.POLICY_AUTOMATIC)
        self.consolescroll.add(self.console)
        self.consoleexpand = gtk.Expander("Debug Console")
        self.consoleexpand.add(self.consolescroll)
        self.mainbox.pack_end(self.consoleexpand, False, False, 0)
        self.console.show()
        self.consolescroll.show()
        self.consoleexpand.show()

        # disable our controls by default at start
        self.set_main_sensitive(False)
        self.set_zone2_sensitive(False)
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
