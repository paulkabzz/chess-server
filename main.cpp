#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QStringList>
#include <QFont>
#include <QByteArray>
#include <QTimer>
#include <QDateTime>
#include <QVector>
#include <QtMath>

// ---------- geometry / encoding helpers -------------------------------------

static const char *INIT_BOARD =
    "rnbqkbnr""pppppppp""........""........"
    "........""........""PPPPPPPP""RNBQKBNR";

static QChar glyphFor(char p) {
    switch (p) {
        case 'K': return QChar(0x2654); case 'Q': return QChar(0x2655);
        case 'R': return QChar(0x2656); case 'B': return QChar(0x2657);
        case 'N': return QChar(0x2658); case 'P': return QChar(0x2659);
        case 'k': return QChar(0x265A); case 'q': return QChar(0x265B);
        case 'r': return QChar(0x265C); case 'b': return QChar(0x265D);
        case 'n': return QChar(0x265E); case 'p': return QChar(0x265F);
    }
    return QChar();
}

static QString idxToSq(int idx) {
    return QString(QChar('a' + (idx % 8))) + QChar('8' - (idx / 8));
}
static int sqToIdx(const QString &sq) {
    if (sq.size() != 2) return -1;
    int c = sq[0].toLatin1() - 'a';
    int r = 8 - (sq[1].toLatin1() - '0');
    if (c < 0 || c > 7 || r < 0 || r > 7) return -1;
    return r * 8 + c;
}

struct Move {
    int src, dst;
    char piece;
    char captured;      // '.' when not a capture
};

// Reconstruct which moves caused (from -> to). Handles promotions & captures.
static QVector<Move> diffMoves(const QByteArray &from, const QByteArray &to) {
    QVector<int> empties;   // squares that became empty
    QVector<int> filled;    // squares that changed to something non-'.'
    for (int i = 0; i < 64; ++i) {
        if (from[i] == to[i]) continue;
        if (from[i] != '.' && to[i] == '.')      empties.append(i);
        else if (to[i] != '.')                   filled.append(i);
    }
    QVector<Move> moves;
    for (int dst : filled) {
        char target = to[dst];
        for (int j = 0; j < empties.size(); ++j) {
            int src = empties[j];
            char srcPiece = from[src];
            bool match = (srcPiece == target)
                      || (srcPiece == 'P' && target == 'Q')
                      || (srcPiece == 'p' && target == 'q');
            if (match) {
                moves.append({src, dst, srcPiece, from[dst]});
                empties.removeAt(j);
                break;
            }
        }
    }
    return moves;
}

// ---------- BoardWidget -----------------------------------------------------

class BoardWidget : public QWidget {
    Q_OBJECT
public:
    explicit BoardWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        board_ = QByteArray(64, '.');
        animTimer_.setInterval(16);
        connect(&animTimer_, &QTimer::timeout, this, &BoardWidget::onAnimTick);
    }

    // Instant update (no animation). Use for fresh loads, resets, disconnects.
    void setBoard(const QByteArray &b) {
        if (b.size() != 64) return;
        board_ = b;
        cancelAnimation();
        update();
    }

    // Animated update. PendingSrc/Dst is the move the user just requested,
    // so an AI reply that arrives in the same frame is animated *after* it.
    void transitionTo(const QByteArray &newBoard,
                      int pendingSrc = -1, int pendingDst = -1) {
        if (newBoard.size() != 64) return;
        QByteArray oldBoard = board_;
        board_ = newBoard;

        if (oldBoard == newBoard) { update(); return; }

        QVector<Move> moves = diffMoves(oldBoard, newBoard);

        // Snap for anything that isn't a clean 1- or 2-move transition
        // (new game, board-button reset, malformed state).
        if (moves.isEmpty() || moves.size() > 2) {
            cancelAnimation();
            update();
            return;
        }

        // Order: user's pending move first (if any), then AI reply.
        QVector<Move> ordered;
        if (pendingSrc >= 0 && pendingDst >= 0) {
            for (int i = 0; i < moves.size(); ++i) {
                if (moves[i].src == pendingSrc && moves[i].dst == pendingDst) {
                    ordered.append(moves.takeAt(i));
                    break;
                }
            }
        }
        ordered.append(moves);

        animSeq_.clear();
        QByteArray cur = oldBoard;
        for (const Move &m : ordered) {
            QByteArray nxt = cur;
            nxt[m.dst] = nxt[m.src];
            nxt[m.src] = '.';
            if (m.piece == 'P' && m.dst / 8 == 0) nxt[m.dst] = 'Q';
            if (m.piece == 'p' && m.dst / 8 == 7) nxt[m.dst] = 'q';

            AnimStep s;
            s.fromBoard = cur;
            s.toBoard = nxt;
            s.src = m.src;
            s.dst = m.dst;
            s.piece = m.piece;
            s.captured = m.captured;
            animSeq_.append(s);
            cur = nxt;
        }
        if (cur != newBoard) {
            AnimStep s;
            s.fromBoard = cur;
            s.toBoard = newBoard;
            s.src = -1; s.dst = -1;
            animSeq_.append(s);
        }

        animStep_ = 0;
        animStartMs_ = QDateTime::currentMSecsSinceEpoch();
        animating_ = true;
        animTimer_.start();
        update();
    }

    void cancelAnimation() {
        animTimer_.stop();
        animating_ = false;
        animSeq_.clear();
        animStep_ = 0;
        update();
    }

    void setSelected(int idx)          { selected_ = idx; update(); }
    void setLegal(const QSet<int> &d)  { legal_ = d; update(); }
    void setLastMove(const QSet<int> &s) { last_ = s; update(); }

signals:
    void clicked(int idx);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *ev) override;

private slots:
    void onAnimTick() {
        if (!animating_ || animStep_ >= animSeq_.size()) {
            animTimer_.stop();
            animating_ = false;
            update();
            return;
        }
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - animStartMs_ >= STEP_MS) {
            animStep_++;
            animStartMs_ = now;
            if (animStep_ >= animSeq_.size()) {
                animating_ = false;
                animTimer_.stop();
            }
        }
        update();
    }

private:
    struct AnimStep {
        QByteArray fromBoard;
        QByteArray toBoard;
        int src;
        int dst;
        char piece;
        char captured;
    };

    static constexpr int STEP_MS = 180;   // duration of each move
    static constexpr int SQ_MIN  = 32;

    QByteArray board_;
    int        selected_ = -1;
    QSet<int>  legal_;
    QSet<int>  last_;

    QVector<AnimStep> animSeq_;
    QTimer     animTimer_;
    bool       animating_ = false;
    int        animStep_ = 0;
    qint64     animStartMs_ = 0;
};

void BoardWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    int side = qMin(width(), height());
    int sq = side / 8;
    int offX = (width() - side) / 2;
    int offY = (height() - side) / 2;

    static const QColor light("#ebecd0");
    static const QColor dark("#739552");
    QColor lastTint(247, 247, 105, 170);
    QColor selTint(130, 151, 105, 200);
    QColor legalDot("#2f2f2f");

    // Board squares + highlights
    for (int i = 0; i < 64; ++i) {
        int r = i / 8, c = i % 8;
        QRect rc(offX + c * sq, offY + r * sq, sq, sq);
        p.fillRect(rc, ((r + c) % 2 == 0) ? light : dark);
        if (last_.contains(i)) p.fillRect(rc, lastTint);
        if (selected_ == i)    p.fillRect(rc, selTint);
    }

    // Determine which board to render
    QByteArray renderBoard = board_;
    int moveSrc = -1, moveDst = -1;
    char movingPiece = '.';
    char capturedPiece = '.';
    float t = 0.0f;

    if (animating_ && animStep_ < animSeq_.size()) {
        const AnimStep &step = animSeq_[animStep_];
        renderBoard = step.fromBoard;
        moveSrc = step.src;
        moveDst = step.dst;
        movingPiece = step.piece;
        capturedPiece = step.captured;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        float raw = float(now - animStartMs_) / float(STEP_MS);
        raw = qBound(0.0f, raw, 1.0f);
        t = 1.0f - qPow(1.0f - raw, 3.0f);   // ease-out cubic
    }

    QFont base("DejaVu Sans");
    base.setPixelSize(qMax(12, int(sq * 0.63)));

    // Static pieces from renderBoard, skipping the animated move
    for (int i = 0; i < 64; ++i) {
        if (i == moveSrc) continue;
        if (moveDst >= 0 && i == moveDst && capturedPiece != '.') continue;
        char ch = renderBoard.at(i);
        if (ch == '.') continue;
        QChar glyph = glyphFor(ch);
        QRect rc(offX + (i % 8) * sq, offY + (i / 8) * sq, sq, sq);
        bool white = (ch >= 'A' && ch <= 'Z');
        p.setFont(base);
        if (white) {
            p.setPen(QColor("#5a5a5a"));
            p.drawText(rc.translated(1, 1), Qt::AlignCenter, QString(glyph));
            p.setPen(QColor("#fafafa"));
        } else {
            p.setPen(QColor("#151515"));
        }
        p.drawText(rc, Qt::AlignCenter, QString(glyph));
    }

    // Moving piece at interpolated position
    if (moveSrc >= 0 && moveDst >= 0 && movingPiece != '.') {
        QPointF a(offX + (moveSrc % 8) * sq + sq / 2.0,
                  offY + (moveSrc / 8) * sq + sq / 2.0);
        QPointF b(offX + (moveDst % 8) * sq + sq / 2.0,
                  offY + (moveDst / 8) * sq + sq / 2.0);
        QPointF c = a + (b - a) * t;
        QRect rc(int(c.x() - sq / 2), int(c.y() - sq / 2), sq, sq);
        QChar glyph = glyphFor(movingPiece);
        bool white = (movingPiece >= 'A' && movingPiece <= 'Z');
        p.setFont(base);
        if (white) {
            p.setPen(QColor("#5a5a5a"));
            p.drawText(rc.translated(1, 1), Qt::AlignCenter, QString(glyph));
            p.setPen(QColor("#fafafa"));
        } else {
            p.setPen(QColor("#151515"));
        }
        p.drawText(rc, Qt::AlignCenter, QString(glyph));
    }

    // Shrinking captured piece
    if (moveDst >= 0 && capturedPiece != '.') {
        float scale = 1.0f - t;
        int px = qMax(1, int(sq * 0.63 * scale));
        QFont small("DejaVu Sans");
        small.setPixelSize(px);
        p.setFont(small);
        QRect rc(offX + (moveDst % 8) * sq, offY + (moveDst / 8) * sq, sq, sq);
        QChar glyph = glyphFor(capturedPiece);
        bool white = (capturedPiece >= 'A' && capturedPiece <= 'Z');
        p.setPen(white ? QColor("#fafafa") : QColor("#151515"));
        p.drawText(rc, Qt::AlignCenter, QString(glyph));
    }

    // Legal-move indicators
    p.setFont(base);
    for (int dst : legal_) {
        QRect rc(offX + (dst % 8) * sq, offY + (dst / 8) * sq, sq, sq);
        if (board_.at(dst) == '.') {
            int r = qMax(6, sq / 9);
            p.setPen(Qt::NoPen);
            p.setBrush(legalDot);
            p.drawEllipse(rc.center(), r, r);
        } else {
            QPen pen(legalDot, qMax(3, sq / 18));
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            int r = sq / 2 - 3;
            p.drawEllipse(rc.center(), r, r);
        }
    }
}

void BoardWidget::mousePressEvent(QMouseEvent *ev) {
    if (ev->button() != Qt::LeftButton) return;
    // If the user clicked during an animation, snap to the final state so
    // their input is applied against the authoritative board.
    if (animating_) cancelAnimation();

    int side = qMin(width(), height());
    int sq = side / 8;
    int offX = (width() - side) / 2;
    int offY = (height() - side) / 2;
    QPoint pt = ev->pos();
    int x = pt.x() - offX;
    int y = pt.y() - offY;
    if (x < 0 || y < 0 || x >= side || y >= side) return;
    int c = qBound(0, x / sq, 7);
    int r = qBound(0, y / sq, 7);
    emit clicked(r * 8 + c);
}

// ---------- MainWindow ------------------------------------------------------

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setupUi();
        connect(&serial_, &QSerialPort::readyRead,
                this, &MainWindow::onReadyRead);
        connect(&serial_, &QSerialPort::errorOccurred,
                this, &MainWindow::onSerialError);
    }

    void setPortText(const QString &port) {
        if (portBox_) portBox_->setCurrentText(port);
    }

private slots:
    void onConnect();
    void onDisconnect();
    void onNewGame()   { startGame(false); }
    void onNewGameAI() { startGame(true);  }
    void onResign();
    void onSquareClicked(int idx);
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError err);
    void refreshPorts();

private:
    void setupUi();
    void startGame(bool ai);
    void send(const QString &cmd);
    void handleLine(const QString &line);
    void applyBoard(const QString &board, const QString &turn,
                    int pendingSrc = -1, int pendingDst = -1);
    void endGame(const QString &reason);
    void showNack(const QString &code);
    void updateConnectionState();
    void refreshButtons();
    void updateTurnLabel();

    QComboBox   *portBox_ = nullptr;
    QPushButton *btnRefresh_ = nullptr;
    QPushButton *btnConnect_ = nullptr;
    QPushButton *btnDisconnect_ = nullptr;
    QPushButton *btnNew_ = nullptr;
    QPushButton *btnNewAI_ = nullptr;
    QPushButton *btnResign_ = nullptr;
    QLabel      *turnLabel_ = nullptr;
    QLabel      *statusLabel_ = nullptr;
    BoardWidget *board_ = nullptr;

    QSerialPort serial_;
    QByteArray  rxBuf_;

    QByteArray  boardBytes_ = QByteArray(64, '.');
    QString     turn_ = "-";
    bool        over_ = true;
    int         selected_ = -1;
    QSet<int>   legal_;
    QSet<int>   lastMove_;
    int         pendingSrc_ = -1;
    int         pendingDst_ = -1;
};

void MainWindow::setupUi() {
    auto *central = new QWidget(this);
    auto *v = new QVBoxLayout(central);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel("Port:", central));

    portBox_ = new QComboBox(central);
    portBox_->setEditable(true);
    portBox_->setMinimumWidth(260);
    top->addWidget(portBox_);

    btnRefresh_ = new QPushButton("⟳", central);
    btnRefresh_->setToolTip("Refresh serial ports");
    btnRefresh_->setFixedWidth(36);
    top->addWidget(btnRefresh_);

    btnConnect_ = new QPushButton("Connect", central);
    btnDisconnect_ = new QPushButton("Disconnect", central);
    btnDisconnect_->setEnabled(false);
    top->addWidget(btnConnect_);
    top->addWidget(btnDisconnect_);
    top->addStretch();
    v->addLayout(top);

    auto *bar = new QHBoxLayout();
    btnNew_ = new QPushButton("New Game", central);
    btnNewAI_ = new QPushButton("New vs AI", central);
    btnResign_ = new QPushButton("Resign", central);
    btnNew_->setEnabled(false);
    btnNewAI_->setEnabled(false);
    btnResign_->setEnabled(false);
    bar->addWidget(btnNew_);
    bar->addWidget(btnNewAI_);
    bar->addWidget(btnResign_);
    bar->addStretch();
    turnLabel_ = new QLabel("Turn: —", central);
    QFont tf = turnLabel_->font();
    tf.setBold(true);
    tf.setPointSize(tf.pointSize() + 2);
    turnLabel_->setFont(tf);
    bar->addWidget(turnLabel_);
    v->addLayout(bar);

    board_ = new BoardWidget(central);
    v->addWidget(board_, 1);

    statusLabel_ = new QLabel("Not connected", central);
    v->addWidget(statusLabel_);

    setCentralWidget(central);
    resize(1200, 950);

    connect(btnRefresh_,    &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(btnConnect_,    &QPushButton::clicked, this, &MainWindow::onConnect);
    connect(btnDisconnect_, &QPushButton::clicked, this, &MainWindow::onDisconnect);
    connect(btnNew_,        &QPushButton::clicked, this, &MainWindow::onNewGame);
    connect(btnNewAI_,      &QPushButton::clicked, this, &MainWindow::onNewGameAI);
    connect(btnResign_,     &QPushButton::clicked, this, &MainWindow::onResign);
    connect(board_,         &BoardWidget::clicked, this, &MainWindow::onSquareClicked);

    refreshPorts();
}

void MainWindow::refreshPorts() {
    QString previousText = portBox_->currentText();
    portBox_->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    int bestIndex = -1;

    for (int i = 0; i < ports.size(); ++i) {
        const QSerialPortInfo &info = ports[i];
        QString label = info.systemLocation();
        if (!info.description().isEmpty()) {
            label += QString(" (%1)").arg(info.description());
        }
        portBox_->addItem(label, info.systemLocation());

        // Auto-select STM32 (Vendor ID 0x0483) or common USB-UART adapters
        if (info.vendorIdentifier() == 0x0483 ||
            info.description().contains("STLink", Qt::CaseInsensitive) ||
            info.description().contains("ST-Link", Qt::CaseInsensitive) ||
            info.description().contains("UART", Qt::CaseInsensitive) ||
            info.description().contains("USB", Qt::CaseInsensitive) ||
            info.portName().contains("usb", Qt::CaseInsensitive) ||
            info.portName().contains("ACM", Qt::CaseInsensitive)) {
            bestIndex = i;
        }
    }

    if (bestIndex >= 0) {
        portBox_->setCurrentIndex(bestIndex);
    } else if (!previousText.isEmpty()) {
        portBox_->setCurrentText(previousText);
    } else if (portBox_->count() == 0) {
        portBox_->setCurrentText("/dev/ttyACM0");
    }
}

void MainWindow::updateConnectionState() {
    if (serial_.isOpen()) {
        btnConnect_->setStyleSheet(
            "QPushButton {"
            "  background-color: #4CAF50;"
            "  color: white;"
            "  font-weight: bold;"
            "  border-radius: 4px;"
            "  padding: 4px 10px;"
            "}");
        btnConnect_->setText("● Connected");
    } else {
        btnConnect_->setStyleSheet("");
        btnConnect_->setText("Connect");
    }
    refreshButtons();
}

void MainWindow::refreshButtons() {
    bool connected = serial_.isOpen();
    btnConnect_->setEnabled(!connected);
    portBox_->setEnabled(!connected);
    btnRefresh_->setEnabled(!connected);
    btnDisconnect_->setEnabled(connected);
    btnNew_->setEnabled(connected);
    btnNewAI_->setEnabled(connected);
    btnResign_->setEnabled(connected && !over_);
}

void MainWindow::updateTurnLabel() {
    if (turn_ == "-" || turn_.isEmpty())
        turnLabel_->setText("Turn: —");
    else
        turnLabel_->setText(QString("Turn: %1")
            .arg(turn_ == "W" ? "White" : "Black"));
}

void MainWindow::onConnect() {
    // Use stored systemLocation if selected from dropdown, otherwise parse typed text
    QString port = portBox_->currentData().toString();
    if (port.isEmpty() || !portBox_->currentText().startsWith(port)) {
        port = portBox_->currentText().section(' ', 0, 0).trimmed();
    }

    serial_.setPortName(port);
    serial_.setBaudRate(115200);
    serial_.setDataBits(QSerialPort::Data8);
    serial_.setParity(QSerialPort::NoParity);
    serial_.setStopBits(QSerialPort::OneStop);
    serial_.setFlowControl(QSerialPort::NoFlowControl);

    if (!serial_.open(QIODevice::ReadWrite)) {
        statusLabel_->setText(
            QString("Cannot open %1: %2").arg(port, serial_.errorString()));
        return;
    }
    updateConnectionState();
    statusLabel_->setText(
        "Connected — press New Game or New vs AI "
        "(or the board's user button)");
    send("CONNECT");
}

void MainWindow::onDisconnect() {
    if (serial_.isOpen()) {
        send("DISCONNECT");
        serial_.flush();
        serial_.waitForBytesWritten(100);
        serial_.close();
    }
    boardBytes_ = QByteArray(64, '.');
    turn_ = "-";
    over_ = true;
    selected_ = -1;
    legal_.clear();
    lastMove_.clear();
    pendingSrc_ = pendingDst_ = -1;
    board_->setBoard(boardBytes_);
    board_->setSelected(-1);
    board_->setLegal({});
    board_->setLastMove({});
    updateTurnLabel();
    updateConnectionState();
    statusLabel_->setText("Disconnected");
}

void MainWindow::startGame(bool ai) {
    over_ = true;
    selected_ = -1;
    legal_.clear();
    lastMove_.clear();
    pendingSrc_ = pendingDst_ = -1;
    board_->setSelected(-1);
    board_->setLegal({});
    board_->setLastMove({});
    statusLabel_->setText("Starting new game…");
    send("DISCONNECT");
    send(ai ? "NEW:AI" : "NEW");
}

void MainWindow::onResign() {
    if (over_) return;
    send("RESIGN");
    over_ = true;
    selected_ = -1;
    legal_.clear();
    board_->setSelected(-1);
    board_->setLegal({});
    refreshButtons();
}

void MainWindow::onSquareClicked(int idx) {
    if (over_ || !serial_.isOpen()) return;

    if (selected_ < 0) {
        selected_ = idx;
        legal_.clear();
        board_->setSelected(idx);
        board_->setLegal({});
        send(QString("MOVES:%1").arg(idxToSq(idx)));
        return;
    }
    if (idx == selected_) {
        selected_ = -1;
        legal_.clear();
        board_->setSelected(-1);
        board_->setLegal({});
        return;
    }
    int src = selected_;
    selected_ = -1;
    legal_.clear();
    board_->setSelected(-1);
    board_->setLegal({});
    pendingSrc_ = src;
    pendingDst_ = idx;
    send(QString("MOVE:%1%2").arg(idxToSq(src), idxToSq(idx)));
}

void MainWindow::send(const QString &cmd) {
    if (!serial_.isOpen()) return;
    serial_.write((cmd + "\n").toUtf8());
    serial_.flush();
}

void MainWindow::onReadyRead() {
    rxBuf_ += serial_.readAll();
    int nl;
    while ((nl = rxBuf_.indexOf('\n')) >= 0) {
        QByteArray line = rxBuf_.left(nl);
        rxBuf_.remove(0, nl + 1);
        QString s = QString::fromLatin1(line).trimmed();
        if (!s.isEmpty()) handleLine(s);
    }
}

void MainWindow::onSerialError(QSerialPort::SerialPortError err) {
    if (err == QSerialPort::NoError) return;
    if (err == QSerialPort::ResourceError || err == QSerialPort::DeviceNotFoundError) {
        statusLabel_->setText(QString("Serial error: %1").arg(serial_.errorString()));
        onDisconnect();
    }
}

void MainWindow::handleLine(const QString &line) {
    if (line.startsWith("OK:BOARD:") || line.startsWith("BOARD:")) {
        int prefixLen = line.startsWith("OK:BOARD:") ? 9 : 6;
        QStringList parts = line.mid(prefixLen).split(":");
        if (parts.size() >= 3 && parts[0].size() == 64) {
            bool wasReset = (parts[0] == QString(INIT_BOARD)
                && boardBytes_ != QByteArray(64, '.')
                && boardBytes_ != QByteArray(INIT_BOARD));
            applyBoard(parts[0], parts[2], pendingSrc_, pendingDst_);
            pendingSrc_ = pendingDst_ = -1;
            if (wasReset)
                statusLabel_->setText(
                    "New game started (from board button) — White to move");
        }
        return;
    }
    if (line.startsWith("WIN:")) {
        QStringList parts = line.split(":");
        QString winner = parts.size() > 1 ? parts[1] : "?";
        int bi = parts.indexOf("BOARD");
        if (bi >= 0 && bi + 1 < parts.size())
            applyBoard(parts[bi + 1], "-", pendingSrc_, pendingDst_);
        pendingSrc_ = pendingDst_ = -1;
        endGame(QString("%1 wins by checkmate")
            .arg(winner == "W" ? "White" : "Black"));
        return;
    }
    if (line.startsWith("DRAW:")) {
        QStringList parts = line.split(":");
        int bi = parts.indexOf("BOARD");
        if (bi >= 0 && bi + 1 < parts.size())
            applyBoard(parts[bi + 1], "-", pendingSrc_, pendingDst_);
        pendingSrc_ = pendingDst_ = -1;
        endGame("Draw (stalemate / 50-move / insufficient material)");
        return;
    }
    if (line.startsWith("OK:GAMEOVER:")) {
        QString w = line.split(":").last();
        pendingSrc_ = pendingDst_ = -1;
        endGame(QString("%1 wins by resignation")
            .arg(w == "W" ? "White" : "Black"));
        return;
    }
    if (line.startsWith("VALID:")) {
        if (selected_ >= 0) {
            QString payload = line.mid(6);
            legal_.clear();
            if (!payload.isEmpty()) {
                for (const QString &s : payload.split(",")) {
                    int idx = sqToIdx(s);
                    if (idx >= 0) legal_.insert(idx);
                }
            }
            board_->setLegal(legal_);
        }
        return;
    }
    if (line == "OK") { statusLabel_->setText("Ready"); return; }
    if (line.startsWith("NACK:")) {
        pendingSrc_ = pendingDst_ = -1;
        showNack(line.mid(5));
        return;
    }
    statusLabel_->setText(QString("Unhandled: %1").arg(line));
}

void MainWindow::applyBoard(const QString &board, const QString &turn,
                            int pendingSrc, int pendingDst) {
    QByteArray newBoard = board.toLatin1();
    if (newBoard.size() != 64) return;

    if (boardBytes_ == QByteArray(64, '.')) {
        lastMove_.clear();
    } else {
        lastMove_.clear();
        for (int i = 0; i < 64; ++i)
            if (boardBytes_.at(i) != newBoard.at(i)) lastMove_.insert(i);
    }

    QByteArray oldBoard = boardBytes_;
    boardBytes_ = newBoard;
    if (turn == "W" || turn == "B") turn_ = turn;
    over_ = false;

    // Board widget animates from its current state to the new one.
    if (oldBoard == QByteArray(64, '.')
        || newBoard == QByteArray(INIT_BOARD)
        || oldBoard == newBoard) {
        board_->setBoard(boardBytes_);
    } else {
        board_->transitionTo(boardBytes_, pendingSrc, pendingDst);
    }

    board_->setLastMove(lastMove_);
    board_->setSelected(-1);
    board_->setLegal({});

    updateTurnLabel();
    refreshButtons();
    statusLabel_->setText(
        QString("%1 to move").arg(turn_ == "W" ? "White" : "Black"));
}

void MainWindow::endGame(const QString &reason) {
    over_ = true;
    selected_ = -1;
    legal_.clear();
    board_->setSelected(-1);
    board_->setLegal({});
    refreshButtons();
    statusLabel_->setText(QString("Game over — %1").arg(reason));
}

void MainWindow::showNack(const QString &code) {
    QString msg;
    if (code == "NOGAME")      msg = "No game in progress — press New Game.";
    else if (code == "RANGE")  msg = "Move out of range — invalid square or format.";
    else if (code == "EMPTY")  msg = "Source square is empty.";
    else if (code == "NOTURN") msg = "That piece is not yours to move.";
    else if (code == "OCCUPIED") msg = "Destination square is occupied by your piece.";
    else if (code == "ILLEGAL")  msg = "Illegal move for that piece (or leaves king in check).";
    else if (code == "BADCMD")   msg = "Malformed or unknown command.";
    else if (code == "TIMEOUT")  msg = "Incomplete frame timed out.";
    else msg = code;
    statusLabel_->setText(QString("Rejected — %1").arg(msg));
}

// ---------- entry point -----------------------------------------------------

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    if (argc > 1) {
        w.setPortText(QString::fromLocal8Bit(argv[1]));
    }
    return app.exec();
}

#include "main.moc"