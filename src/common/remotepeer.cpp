/***************************************************************************
 *   Copyright (C) 2005-2015 by the Quassel Project                        *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/

#include <QtEndian>

#include <QHostAddress>
#include <QTimer>

#ifdef HAVE_SSL
#  include <QSslSocket>
#else
#  include <QTcpSocket>
#endif

#include "remotepeer.h"

using namespace Protocol;

const quint32 maxMessageSize = 64 * 1024 * 1024; // This is uncompressed size. 64 MB should be enough for any sort of initData or backlog chunk

RemotePeer::RemotePeer(::AuthHandler *authHandler, QTcpSocket *socket, Compressor::CompressionLevel level, QObject *parent)
    : Peer(authHandler, parent),
    _socket(socket),
    _compressor(new Compressor(socket, level, this)),
    _signalProxy(0),
    _heartBeatTimer(new QTimer(this)),
    _heartBeatCount(0),
    _lag(0),
    _msgSize(0)
{
    socket->setParent(this);
    connect(socket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), SLOT(onSocketStateChanged(QAbstractSocket::SocketState)));
    connect(socket, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(onSocketError(QAbstractSocket::SocketError)));
    connect(socket, SIGNAL(disconnected()), SIGNAL(disconnected()));

#ifdef HAVE_SSL
    QSslSocket *sslSocket = qobject_cast<QSslSocket *>(socket);
    if (sslSocket)
        connect(sslSocket, SIGNAL(encrypted()), SIGNAL(secureStateChanged()));
#endif

    connect(_compressor, SIGNAL(readyRead()), SLOT(onReadyRead()));
    connect(_compressor, SIGNAL(error(Compressor::Error)), SLOT(onCompressionError(Compressor::Error)));

    connect(_heartBeatTimer, SIGNAL(timeout()), SLOT(sendHeartBeat()));
}


void RemotePeer::onSocketStateChanged(QAbstractSocket::SocketState state)
{
    if (state == QAbstractSocket::ClosingState) {
        emit statusMessage(tr("Disconnecting..."));
    }
}


void RemotePeer::onSocketError(QAbstractSocket::SocketError error)
{
    emit socketError(error, socket()->errorString());
}


void RemotePeer::onCompressionError(Compressor::Error error)
{
    close(QString("Compression error %1").arg(error));
}


QString RemotePeer::description() const
{
    if (socket())
        return socket()->peerAddress().toString();

    return QString();
}


::SignalProxy *RemotePeer::signalProxy() const
{
    return _signalProxy;
}


void RemotePeer::setSignalProxy(::SignalProxy *proxy)
{
    if (proxy == _signalProxy)
        return;

    if (!proxy) {
        _heartBeatTimer->stop();
        disconnect(signalProxy(), 0, this, 0);
        _signalProxy = 0;
        if (isOpen())
            close();
    }
    else {
        if (signalProxy()) {
            qWarning() << Q_FUNC_INFO << "Setting another SignalProxy not supported, ignoring!";
            return;
        }
        _signalProxy = proxy;
        connect(proxy, SIGNAL(heartBeatIntervalChanged(int)), SLOT(changeHeartBeatInterval(int)));
        _heartBeatTimer->setInterval(proxy->heartBeatInterval() * 1000);
        _heartBeatTimer->start();
    }
}


void RemotePeer::changeHeartBeatInterval(int secs)
{
    if(secs <= 0)
        _heartBeatTimer->stop();
    else {
        _heartBeatTimer->setInterval(secs * 1000);
        _heartBeatTimer->start();
    }
}


int RemotePeer::lag() const
{
    return _lag;
}


QTcpSocket *RemotePeer::socket() const
{
    return _socket;
}


bool RemotePeer::isSecure() const
{
    if (socket()) {
        if (isLocal())
            return true;
#ifdef HAVE_SSL
        QSslSocket *sslSocket = qobject_cast<QSslSocket *>(socket());
        if (sslSocket && sslSocket->isEncrypted())
            return true;
#endif
    }
    return false;
}


bool RemotePeer::isLocal() const
{
    if (socket()) {
        if (socket()->peerAddress() == QHostAddress::LocalHost || socket()->peerAddress() == QHostAddress::LocalHostIPv6)
            return true;
    }
    return false;
}


bool RemotePeer::isOpen() const
{
    return socket() && socket()->state() == QTcpSocket::ConnectedState;
}


void RemotePeer::close(const QString &reason)
{
    if (!reason.isEmpty()) {
        qWarning() << "Disconnecting:" << reason;
    }

    if (socket() && socket()->state() != QTcpSocket::UnconnectedState) {
        socket()->disconnectFromHost();
    }
}


void RemotePeer::onReadyRead()
{
    QByteArray msg;
    while (readMessage(msg))
        processMessage(msg);
}


bool RemotePeer::readMessage(QByteArray &msg)
{
    if (_msgSize == 0) {
        if (_compressor->bytesAvailable() < 4)
            return false;
        _compressor->read((char*)&_msgSize, 4);
        _msgSize = qFromBigEndian<quint32>(_msgSize);

        if (_msgSize > maxMessageSize) {
            close("Peer tried to send package larger than max package size!");
            return false;
        }

        if (_msgSize == 0) {
            close("Peer tried to send an empty message!");
            return false;
        }
    }

    if (_compressor->bytesAvailable() < _msgSize) {
        emit transferProgress(socket()->bytesAvailable(), _msgSize);
        return false;
    }

    emit transferProgress(_msgSize, _msgSize);

    msg.resize(_msgSize);
    qint64 bytesRead = _compressor->read(msg.data(), _msgSize);
    if (bytesRead != _msgSize) {
        close("Premature end of data stream!");
        return false;
    }

    _msgSize = 0;
    return true;
}


void RemotePeer::writeMessage(const QByteArray &msg)
{
    quint32 size = qToBigEndian<quint32>(msg.size());
    _compressor->write((const char*)&size, 4, Compressor::NoFlush);
    _compressor->write(msg.constData(), msg.size());
}


void RemotePeer::handle(const HeartBeat &heartBeat)
{
    dispatch(HeartBeatReply(heartBeat.timestamp));
}


void RemotePeer::handle(const HeartBeatReply &heartBeatReply)
{
    _heartBeatCount = 0;
#if QT_VERSION >= 0x040700
    emit lagUpdated(heartBeatReply.timestamp.msecsTo(QDateTime::currentDateTime().toUTC()) / 2);
#else
    emit lagUpdated(heartBeatReply.timestamp.time().msecsTo(QDateTime::currentDateTime().toUTC().time()) / 2);
#endif
}


void RemotePeer::sendHeartBeat()
{
    if (signalProxy()->maxHeartBeatCount() > 0 && _heartBeatCount >= signalProxy()->maxHeartBeatCount()) {
        qWarning() << "Disconnecting peer:" << description()
                   << "(didn't receive a heartbeat for over" << _heartBeatCount *_heartBeatTimer->interval() / 1000 << "seconds)";
        socket()->close();
        _heartBeatTimer->stop();
        return;
    }

    if (_heartBeatCount > 0) {
        _lag = _heartBeatCount * _heartBeatTimer->interval();
        emit lagUpdated(_lag);
    }

    dispatch(HeartBeat(QDateTime::currentDateTime().toUTC()));
    ++_heartBeatCount;
}
