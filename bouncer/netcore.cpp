#include "core.h"
#include "ini.h"


NetCore::NetCore() {
	clientCount = 0;
	for (int i = 0; i < CLIENT_LIMIT; i++)
		clients[i] = NULL;
	serverCount = 0;
	for (int i = 0; i < SERVER_LIMIT; i++)
		servers[i] = NULL;

	nextWindowID = 1;
	nextServerID = 1;
}

Client *NetCore::findClientWithSessionKey(uint8_t *key) const {
	for (int i = 0; i < clientCount; i++)
		if (!memcmp(clients[i]->sessionKey, key, SESSION_KEY_SIZE))
			return clients[i];

	return 0;
}

int NetCore::registerServer(Server *server) {
	if (serverCount >= SERVER_LIMIT)
		return -1;

	server->id = nextServerID;
	nextServerID++;

	int index = serverCount++;
	servers[index] = server;
	server->attachedToCore();

	// tell clients
	Buffer pkt;
	pkt.writeU32(1);
	server->syncStateForClient(pkt);
	sendToClients(Packet::B2C_SERVER_ADD, pkt);

	return index;
}
void NetCore::deregisterServer(Server *server) {
	int index = -1;
	for (int i = 0; i < serverCount; i++) {
		if (servers[i] == server) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return;

	// tell clients
	Buffer pkt;
	pkt.writeU32(1);
	pkt.writeU32(server->id);
	sendToClients(Packet::B2C_SERVER_REMOVE, pkt);

	// get rid of it from our end!
	server->close();
	delete server;

	serverCount--;
	servers[index] = servers[serverCount];
}


Server *NetCore::findServer(int id) const {
	for (int i = 0; i < serverCount; i++)
		if (servers[i]->id == id)
			return servers[i];

	return 0;
}



int NetCore::execute() {
	// prepare the listen socket
	int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == -1) {
		perror("Could not create the listener socket");
		return -1;
	}

	int v = 1;
	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&v, sizeof(v)) == -1) {
		perror("Could not set SO_REUSEADDR");
		return -2;
	}

	sockaddr_in listenAddr;
	listenAddr.sin_family = AF_INET;
	listenAddr.sin_port = htons(5454);
	listenAddr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(listener, (sockaddr *)&listenAddr, sizeof(listenAddr)) == -1) {
		perror("Could not bind to the listener socket");
		return -3;
	}

	if (!SocketRWCommon::setSocketNonBlocking(listener)) {
		perror("[Listener] Could not set non-blocking");
		return -4;
	}

	if (listen(listener, 10) == -1) {
		perror("Could not listen()");
		return -5;
	}

	printf("Listening!\n");


	// do stuff!
	quitFlag = false;

	while (!quitFlag) {
		fd_set readSet, writeSet;
		FD_ZERO(&readSet);
		FD_ZERO(&writeSet);

		int maxFD = listener;
		FD_SET(listener, &readSet);

		time_t now = time(NULL);

		for (int i = 0; i < clientCount; i++) {
#ifdef USE_GNUTLS
			if (clients[i]->state() == Client::CS_TLS_HANDSHAKE)
				clients[i]->tryTLSHandshake();
#endif

			if (clients[i]->sock != -1) {
				if (clients[i]->sock > maxFD)
					maxFD = clients[i]->sock;

				if (clients[i]->state() == Client::CS_CONNECTED)
					FD_SET(clients[i]->sock, &readSet);
				if (clients[i]->outputBuf.size() > 0)
					FD_SET(clients[i]->sock, &writeSet);

			} else {
				// Outdated session, can we kill it?
				if (now >= clients[i]->deadTime) {
					printf("[%ld] Session expired, deleting\n", now);

					// Yep.
					Client *client = clients[i];
					if (client->authState == Client::AS_AUTHED)
						client->sessionEndEvent();
					delete client;

					// If this is the last socket in the list, we can just
					// decrement clientCount and all will be fine.
					clientCount--;

					// Otherwise, we move that pointer into this slot, and
					// we subtract one from i so that we'll process that slot
					// on the next loop iteration.
					if (i != clientCount) {
						clients[i] = clients[clientCount];
						i--;
					}
				}
			}
		}

		for (int i = 0; i < serverCount; i++) {
			if (servers[i]->state() == Server::CS_DISCONNECTED) {
				time_t reconTime = servers[i]->getReconnectTime();
				if ((reconTime != 0) && (now > reconTime))
					servers[i]->doReconnect();

			} else if (servers[i]->state() == Server::CS_WAITING_DNS)
				servers[i]->tryConnectPhase();
#ifdef USE_GNUTLS
			else if (servers[i]->state() == Server::CS_TLS_HANDSHAKE) {
				if (servers[i]->tryTLSHandshake())
					servers[i]->connectedEvent();
			}
#endif

			if (servers[i]->sock != -1) {
				if (servers[i]->sock > maxFD)
					maxFD = servers[i]->sock;

				if (servers[i]->state() == Server::CS_CONNECTED)
					FD_SET(servers[i]->sock, &readSet);
				if (servers[i]->outputBuf.size() > 0 || servers[i]->state() == Server::CS_WAITING_CONNECT)
					FD_SET(servers[i]->sock, &writeSet);
			}
		}

		timeval timeout;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		select(maxFD+1, &readSet, &writeSet, NULL, &timeout);

		now = time(NULL);
		//printf("[%lu select:%d]\n", now, numFDs);


		for (int i = 0; i < clientCount; i++) {
			if (clients[i]->sock != -1) {
				if (FD_ISSET(clients[i]->sock, &writeSet))
					clients[i]->writeAction();

				if (FD_ISSET(clients[i]->sock, &readSet)
#ifdef USE_GNUTLS
					|| clients[i]->hasTlsPendingData()
#endif
					)
				{
					clients[i]->readAction();
				}
			}
		}

		for (int i = 0; i < serverCount; i++) {
			if (servers[i]->sock != -1) {
				if (FD_ISSET(servers[i]->sock, &writeSet)) {
					Server *server = servers[i];

					if (server->state() == Server::CS_WAITING_CONNECT) {
						// Welp, this means we're connected!
						// Maybe.
						// We might have an error condition, in which case,
						// we're screwed.
						bool didSucceed = false;
						int sockErr;
						socklen_t sockErrSize = sizeof(sockErr);

						if (getsockopt(server->sock, SOL_SOCKET, SO_ERROR, (char *)&sockErr, &sockErrSize) == 0) {
							if (sockErr == 0)
								didSucceed = true;
						}

						if (didSucceed) {
							// WE'RE IN fuck yeah
							printf("[%d] Connection succeeded!\n", i);
							server->connectionSuccessful();
						} else {
							// Nope. Nuke it.
							printf("[%d] Connection failed: %d\n", i, sockErr);
							server->close();
						}

					} else {
						server->writeAction();
					}
				}


				if (FD_ISSET(servers[i]->sock, &readSet)
#ifdef USE_GNUTLS
					|| servers[i]->hasTlsPendingData()
#endif
					)
				{
					servers[i]->readAction();
				}
			}
		}



		if (FD_ISSET(listener, &readSet)) {
			// Yay, we have a new connection
			int sock = accept(listener, NULL, NULL);

			if (clientCount >= CLIENT_LIMIT) {
				// We can't accept it.
				printf("Too many connections, we can't accept this one. THIS SHOULD NEVER HAPPEN.\n");
#ifdef _WIN32
				shutdown(sock, SD_BOTH);
				closesocket(sock);
#else
				shutdown(sock, SHUT_RDWR);
				close(sock);
#endif
			} else {
				// Create a new connection
				printf("[%d] New connection, fd=%d\n", clientCount, sock);

				Client *client = constructClient();

				clients[clientCount] = client;
				++clientCount;

				client->startService(sock, SERVE_VIA_TLS);
			}
		}
	}

	// Need to shut down all sockets here
	for (int i = 0; i < serverCount; i++)
		servers[i]->close();

	for (int i = 0; i < clientCount; i++)
		clients[i]->close();

#ifdef _WIN32
	shutdown(listener, SD_BOTH);
	closesocket(listener);
#else
	shutdown(listener, SHUT_RDWR);
	close(listener);
#endif

	for (int i = 0; i < serverCount; i++)
		delete servers[i];
	for (int i = 0; i < clientCount; i++)
		delete clients[i];

	serverCount = clientCount = 0;

	return 0;
}



int NetCore::registerWindow(Window *window) {
	window->id = nextWindowID;
	nextWindowID++;

	windows.push_back(window);


	Buffer pkt;
	pkt.writeU32(1);
	window->syncStateForClient(pkt);

	sendToClients(Packet::B2C_WINDOW_ADD, pkt);

	return window->id;
}

void NetCore::deregisterWindow(Window *window) {
	Buffer pkt;
	pkt.writeU32(1);
	pkt.writeU32(window->id);

	sendToClients(Packet::B2C_WINDOW_REMOVE, pkt);

	windows.remove(window);
}

Window *NetCore::findWindow(int id) const {
	std::list<Window *>::const_iterator
		i = windows.begin(),
		e = windows.end();

	for (; i != e; ++i)
		if ((*i)->id == id)
			return *i;

	return 0;
}


void NetCore::sendToClients(Packet::Type type, const Buffer &data) {
	for (int i = 0; i < clientCount; i++)
		if (clients[i]->isAuthed())
			clients[i]->sendPacket(type, data);
}




void NetCore::loadConfig() {
	// Make default config
	int v;

	maxWindowMessageCount = 1000;
	sessionKeepalive = 600;

	auto sections = INI::load("config.ini");

	for (auto &section : sections) {
		if (section.title == "Header") {
			bouncerPassword = section.data["password"];

			v = atoi(section.data["maxWindowMessageCount"].c_str());
			if (v >= 5 && v <= 2000)
				maxWindowMessageCount = v;

			v = atoi(section.data["sessionKeepalive"].c_str());
			if (v >= 1 && v <= 3600)
				sessionKeepalive = v;
		}

		if (section.title == "Server" && serverCount < SERVER_LIMIT) {
			Server *s = constructServer(section.data["type"].c_str());
			if (s) {
				s->loadFromConfig(section.data);
				registerServer(s);
			}
		}
	}
}

void NetCore::saveConfig() {
	std::list<INI::Section> sections;

	INI::Section header;
	header.title = "Header";

	header.data["password"] = bouncerPassword;

	char conv[50];

	sprintf(conv, "%d", maxWindowMessageCount);
	header.data["maxWindowMessageCount"] = conv;
	sprintf(conv, "%d", sessionKeepalive);
	header.data["sessionKeepalive"] = conv;

	sections.push_back(header);

	for (int i = 0; i < serverCount; i++) {
		INI::Section section;
		section.title = "Server";

		servers[i]->saveToConfig(section.data);

		sections.push_back(section);
	}

	INI::save("config.ini", sections);
}




Client *Bouncer::constructClient() {
	return new MobileClient(this);
}

Server *Bouncer::constructServer(const char *type) {
	if (strcmp(type, "IRCServer") == 0)
		return new IRCServer(this);

	return 0;
}



