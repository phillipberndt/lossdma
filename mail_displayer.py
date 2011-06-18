#!/usr/bin/python
import os
import sys
import subprocess
import socket

# Fork into background
if os.fork() != 0:
	sys.exit(0)
if os.fork() != 0:
	sys.exit(0)
os.chdir("/")

# A (quite hackish) function to display a received message
def view_message(mail):
	# Fork again so we don't have to integrate mail receival into the glib main
	# loop
	# TODO Okay this really is not nice. Maybe I should change this sometime..
	pid = os.fork()
	if pid != 0:
		os.waitpid(pid, 0)
		return
	if os.fork() != 0:
		os._exit(0)
	
	# This requires a lot more imports. Especially webkit
	import sys
	import gtk
	import base64
	import re
	import gio
	import gobject
	import poplib
	import email
	import email.header
	import webkit
	import urlparse
	import urllib2
	import email.mime.multipart
	import email.mime.text
	import email.mime.base
	import webbrowser
	import email.encoders
	import dateutil.parser
	currentMessage = email.message_from_string(mail)
	class HTMLPanel(webkit.WebView): #{{{
		def __init__(self):
			super(HTMLPanel, self).__init__()
			self.connect("resource-request-starting", self._uri_handler)
			self.connect("navigation-requested", self._nav_handler)
			self.contentHandler = None
		def load_data(self, data, contentType = "text/plain", encoding = ""):
			if not encoding:
				encoding = "iso-8859-1"
			self.load_string(data, contentType, encoding, "about:user")
		def register_content_handler(self, handler):
			self.contentHandler = handler
		def _uri_handler(self, webView, webFrame, webResource, request, response):
			if request.get_uri()[0:4] == "cid:" and self.contentHandler:
				data, contentType = self.contentHandler(request.get_uri()[4:])
				if data:
					request.set_uri("data:%s;base64,%s" % (contentType, base64.b64encode(data)))
			elif request.get_uri()[0:6] != "about:":
				#print request.get_uri()
				request.set_uri("about:blank")
		def _nav_handler(self, view, frame, request):
			if request.get_uri()[0:6] == "about:":
				return 0
			webbrowser.open(request.get_uri())
			return 1
		#}}}
	def _mail_content_handler(self, cid):
		for x in currentMessage.walk():
			if x["Content-ID"]:
				if x["Content-ID"] == "<%s>" % cid:
					return x.get_payload(None, True), x.get_content_type()
		return None, None
	mail_viewer = gtk.Window()
	mail_viewer.set_size_request(800, 600)
	mail_viewer.set_title("Incoming message" + (": " + currentMessage["subject"] if "subject" in currentMessage else ""))
	scroll = gtk.ScrolledWindow()
	mail_viewer.add(scroll)
	mail_view = HTMLPanel()
	mail_view.register_content_handler(_mail_content_handler)
	scroll.add(mail_view)
	mail_viewer.show_all()
	# Display message
	if currentMessage.is_multipart():
		# Prefer HTML over text/plain
		loaded = False
		for sub in currentMessage.walk():
			if sub.get_content_type() == "text/html":
				currentMessage._displayed_part = sub
				mail_view.load_data(sub.get_payload(None, True), sub.get_content_type(),
				  sub.get_param("charset"))
				loaded = True
				break
		if not loaded:
			for sub in currentMessage.walk():
				if sub.get_content_type() == "text/plain":
					currentMessage._displayed_part = sub
					mail_view.load_data(sub.get_payload(None, True), sub.get_content_type(),
					  sub.get_param("charset"))
					break
	else:
		currentMessage._displayed_part = currentMessage
		mail_view.load_data(currentMessage.get_payload(None, True), currentMessage.get_content_type(),
		  currentMessage.get_param("charset"))
	mail_viewer.connect("hide", lambda *x: gtk.main_quit())
	gtk.main()
	os._exit(0)

if __name__ == "__main__":
	# Create a UDP server on port 12025
	server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	server.bind(("", 12025))
	buffered = {}
	while 1:
		# Receive email messages, terminated with a period
		data, client = server.recvfrom(1024**2)
		if client not in buffered:
			buffered[client] = ""
		buffered[client] += data
		if "\n.\n" in buffered[client]:
			mail, data = buffered[client].split("\n.\n", 2)
			# When one is complete, display it.
			view_message(mail)
			buffered[client] = data
