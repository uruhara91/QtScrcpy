#include "tcpserver.h"
#include "videosocket.h"

TcpServer::TcpServer(QObject *parent) : QTcpServer(parent) {}

TcpServer::~TcpServer() {}

void TcpServer::incomingConnection(qintptr handle)
{
    if (m_isVideoSocket) {
        VideoSocket *socket = new VideoSocket();

        if (socket->setSocketDescriptor(handle)) {
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1); 
            socket->setReadBufferSize(2 * 1024 * 1024);
            
            addPendingConnection(socket);
        } else {
            delete socket;
        }

        m_isVideoSocket = false;
    } else {
        QTcpSocket *socket = new QTcpSocket();
        if (socket->setSocketDescriptor(handle)) {
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
            addPendingConnection(socket);
        } else {
            delete socket;
        }
    }
}