#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QMessageBox>
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

// ---------- Constants & Encoding Helpers ------------------------------------

static const char *INITIAL_BOARD_STATE =
    "rnbqkbnr"
    "pppppppp"
    "........"
    "........"
    "........"
    "........"
    "PPPPPPPP"
    "RNBQKBNR";

static constexpr int PLAYER_MOVE_DURATION_MS   = 180;
static constexpr int AI_THINK_HOLD_MS          = 400;
static constexpr int AI_MOVE_DURATION_MS       = 250;
static constexpr int OUTCOME_POPUP_DELAY_MS    = 350;
static constexpr int HANDSHAKE_TIMEOUT_MS      = 1500;

static QChar getPieceGlyph(char pieceCode) {
    switch (pieceCode) {
        case 'K': return QChar(0x2654); case 'Q': return QChar(0x2655);
        case 'R': return QChar(0x2656); case 'B': return QChar(0x2657);
        case 'N': return QChar(0x2658); case 'P': return QChar(0x2659);
        case 'k': return QChar(0x265A); case 'q': return QChar(0x265B);
        case 'r': return QChar(0x265C); case 'b': return QChar(0x265D);
        case 'n': return QChar(0x265E); case 'p': return QChar(0x265F);
    }
    return QChar();
}

// Converts 0..63 board index to algebraic square (e.g., 0 -> "a8")
static QString indexToSquareString(int boardIndex) {
    int col = boardIndex % 8;
    int row = boardIndex / 8;
    return QString(QChar('a' + col)) + QChar('8' - row);
}

// Converts algebraic square (e.g., "e4") to 0..63 board index
static int squareStringToIndex(const QString &squareStr) {
    if (squareStr.size() != 2) return -1;
    int col = squareStr[0].toLatin1() - 'a';
    int row = 8 - (squareStr[1].toLatin1() - '0');
    if (col < 0 || col > 7 || row < 0 || row > 7) return -1;
    return row * 8 + col;
}

struct ChessMove {
    int sourceIndex;
    int destinationIndex;
    char movingPiece;
    char capturedPiece;
};

// Infers individual moves by comparing squares that emptied vs squares that changed
static QVector<ChessMove> calculateBoardDiffMoves(const QByteArray &previousBoard, const QByteArray &nextBoard) {
    QVector<int> emptiedSquares;
    QVector<int> modifiedSquares;

    for (int i = 0; i < 64; ++i) {
        if (previousBoard[i] == nextBoard[i]) continue;
        if (previousBoard[i] != '.' && nextBoard[i] == '.') {
            emptiedSquares.append(i);
        } else if (nextBoard[i] != '.') {
            modifiedSquares.append(i);
        }
    }

    QVector<ChessMove> detectedMoves;
    for (int destIndex : modifiedSquares) {
        char ResultingPiece = nextBoard[destIndex];
        for (int j = 0; j < emptiedSquares.size(); ++j) {
            int srcIndex = emptiedSquares[j];
            char originalPiece = previousBoard[srcIndex];

            // Match identical pieces or pawn auto-promotions to Queen
            bool isPieceMatch = (originalPiece == ResultingPiece) || (originalPiece == 'P' && ResultingPiece == 'Q') || (originalPiece == 'p' && ResultingPiece == 'q');
            if (isPieceMatch) {
                detectedMoves.append({srcIndex, destIndex, originalPiece, previousBoard[destIndex]});
                emptiedSquares.removeAt(j);
                break;
            }
        }
    }
    return detectedMoves;
}

// ---------- BoardWidget -----------------------------------------------------

class BoardWidget : public QWidget {
    Q_OBJECT

public:
    explicit BoardWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        boardState_ = QByteArray(64, '.');
        animationTimer_.setInterval(16); // ~60 FPS
        connect(&animationTimer_, &QTimer::timeout, this, &BoardWidget::onAnimationTick);
    }

    void setBoardState(const QByteArray &newBoardState) {
        if (newBoardState.size() != 64) return;
        boardState_ = newBoardState;
        cancelAnimation();
        update();
    }

    // Builds a step-by-step animation queue from the old board to the new board
    void transitionToBoard(const QByteArray &newBoardState, int playerSourceIndex = -1, int playerDestIndex = -1) {
        if (newBoardState.size() != 64) return;
        QByteArray previousBoard = boardState_;
        boardState_ = newBoardState;

        if (previousBoard == newBoardState) {
            update();
            return;
        }

        QVector<ChessMove> detectedMoves = calculateBoardDiffMoves(previousBoard, newBoardState);
        // Skip animation on full board resets or complex multi-square diffs (e.g., castling)
        if (detectedMoves.isEmpty() || detectedMoves.size() > 2) {
            cancelAnimation();
            update();
            return;
        }

        // Ensure the human player's move animates before the AI's reply
        QVector<ChessMove> orderedMoves;
        if (playerSourceIndex >= 0 && playerDestIndex >= 0) {
            for (int i = 0; i < detectedMoves.size(); ++i) {
                if (detectedMoves[i].sourceIndex == playerSourceIndex &&
                    detectedMoves[i].destinationIndex == playerDestIndex) {
                    orderedMoves.append(detectedMoves.takeAt(i));
                    break;
                }
            }
        }
        orderedMoves.append(detectedMoves);

        animationSequence_.clear();
        QByteArray simulatedBoard = previousBoard;

        for (int i = 0; i < orderedMoves.size(); ++i) {
            const ChessMove &move = orderedMoves[i];
            QByteArray nextStepBoard = simulatedBoard;
            nextStepBoard[move.destinationIndex] = nextStepBoard[move.sourceIndex];
            nextStepBoard[move.sourceIndex] = '.';

            // Apply visual pawn promotion if reaching the back rank
            if (move.movingPiece == 'P' && move.destinationIndex / 8 == 0) nextStepBoard[move.destinationIndex] = 'Q';
            if (move.movingPiece == 'p' && move.destinationIndex / 8 == 7) nextStepBoard[move.destinationIndex] = 'q';

            AnimationStep step;
            step.startBoard = simulatedBoard;
            step.endBoard = nextStepBoard;
            step.sourceIndex = move.sourceIndex;
            step.destinationIndex = move.destinationIndex;
            step.movingPiece = move.movingPiece;
            step.capturedPiece = move.capturedPiece;

            bool isPlayerMove = (i == 0 && playerSourceIndex >= 0 && playerDestIndex >= 0 && move.sourceIndex == playerSourceIndex && move.destinationIndex == playerDestIndex);

            if (isPlayerMove) {
                step.holdDelayMs = 0;
                step.durationMs = PLAYER_MOVE_DURATION_MS;
            } else {
                step.holdDelayMs = AI_THINK_HOLD_MS;
                step.durationMs = AI_MOVE_DURATION_MS;
            }

            animationSequence_.append(step);
            simulatedBoard = nextStepBoard;
        }

        // Fallback sync if intermediate steps didn't perfectly match the target state
        if (simulatedBoard != newBoardState) {
            AnimationStep syncStep;
            syncStep.startBoard = simulatedBoard;
            syncStep.endBoard = newBoardState;
            animationSequence_.append(syncStep);
        }

        currentAnimationIndex_ = 0;
        animationStartTimestampMs_ = QDateTime::currentMSecsSinceEpoch();
        isAnimating_ = true;
        animationTimer_.start();
        update();
    }

    void cancelAnimation() {
        animationTimer_.stop();
        isAnimating_ = false;
        animationSequence_.clear();
        currentAnimationIndex_ = 0;
        update();
    }

    void setSelectedSquare(int squareIndex) { 
        selectedSquareIndex_ = squareIndex; update(); 
    }

    void setLegalDestinationSquares(const QSet<int> &dest) { 
        legalMoveSquares_ = dest; update();
     }

    void setHighlightedMoveSquares(const QSet<int> &last) { 
        lastMoveSquares_ = last; update(); 
    }

signals:
    void squareClicked(int squareIndex);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private slots:
    void onAnimationTick() {
        if (!isAnimating_ || currentAnimationIndex_ >= animationSequence_.size()) {
            animationTimer_.stop();
            isAnimating_ = false;
            update();
            return;
        }

        const AnimationStep &currentStep = animationSequence_[currentAnimationIndex_];
        qint64 currentTimeMs = QDateTime::currentMSecsSinceEpoch();
        qint64 totalStepTimeMs = currentStep.holdDelayMs + currentStep.durationMs;

        if (currentTimeMs - animationStartTimestampMs_ >= totalStepTimeMs) {
            currentAnimationIndex_++;
            animationStartTimestampMs_ = currentTimeMs;
            if (currentAnimationIndex_ >= animationSequence_.size()) {
                isAnimating_ = false;
                animationTimer_.stop();
            }
        }
        update();
    }

private:
    struct AnimationStep {
        QByteArray startBoard;
        QByteArray endBoard;
        int sourceIndex = -1;
        int destinationIndex = -1;
        char movingPiece = '.';
        char capturedPiece = '.';
        int holdDelayMs = 0;
        int durationMs = PLAYER_MOVE_DURATION_MS;
    };

    QByteArray boardState_;
    int selectedSquareIndex_ = -1;
    QSet<int> legalMoveSquares_;
    QSet<int> lastMoveSquares_;

    QVector<AnimationStep> animationSequence_;
    QTimer animationTimer_;
    bool isAnimating_ = false;
    int currentAnimationIndex_ = 0;
    qint64 animationStartTimestampMs_ = 0;
};

void BoardWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    int boardSideLength = qMin(width(), height());
    int squareSize = boardSideLength / 8;
    int offsetX = (width() - boardSideLength) / 2;
    int offsetY = (height() - boardSideLength) / 2;

    static const QColor lightSquareColor("#ebecd0");
    static const QColor darkSquareColor("#739552");
    QColor lastMoveHighlightColor(247, 247, 105, 170);
    QColor selectedSquareColor(130, 151, 105, 200);
    QColor legalIndicatorColor("#2f2f2f");

    // 1. Draw board squares and highlights
    for (int i = 0; i < 64; ++i) {
        int row = i / 8;
        int col = i % 8;
        QRect squareRect(offsetX + col * squareSize, offsetY + row * squareSize, squareSize, squareSize);

        painter.fillRect(squareRect, ((row + col) % 2 == 0) ? lightSquareColor : darkSquareColor);
        if (lastMoveSquares_.contains(i)) painter.fillRect(squareRect, lastMoveHighlightColor);
        if (selectedSquareIndex_ == i)    painter.fillRect(squareRect, selectedSquareColor);
    }

    // 2. Draw rank (8..1) and file (a..h) coordinate labels
    QFont coordinateFont("DejaVu Sans");
    coordinateFont.setBold(true);
    coordinateFont.setPixelSize(qMax(9, int(squareSize * 0.17)));
    painter.setFont(coordinateFont);

    const int labelPadding = qMax(2, squareSize / 24);

    for (int row = 0; row < 8; ++row) {
        QRect squareRect(offsetX, offsetY + row * squareSize, squareSize, squareSize);
        bool isLightSquare = (row % 2 == 0);
        painter.setPen(isLightSquare ? darkSquareColor : lightSquareColor);
        painter.drawText(squareRect.adjusted(labelPadding, labelPadding, -labelPadding, -labelPadding), Qt::AlignLeft | Qt::AlignTop, QString::number(8 - row));
    }

    for (int col = 0; col < 8; ++col) {
        QRect squareRect(offsetX + col * squareSize, offsetY + 7 * squareSize, squareSize, squareSize);
        bool isLightSquare = ((7 + col) % 2 == 0);
        painter.setPen(isLightSquare ? darkSquareColor : lightSquareColor);
        painter.drawText(squareRect.adjusted(labelPadding, labelPadding, -labelPadding, -labelPadding), Qt::AlignRight | Qt::AlignBottom, QString(QChar('a' + col)));
    }

    // 3. Determine active animation interpolation state
    QByteArray boardToRender = boardState_;
    int animSourceIndex = -1;
    int animDestIndex = -1;
    char animMovingPiece = '.';
    char animCapturedPiece = '.';
    float easedProgress = 0.0f;

    if (isAnimating_ && currentAnimationIndex_ < animationSequence_.size()) {
        const AnimationStep &step = animationSequence_[currentAnimationIndex_];
        boardToRender = step.startBoard;
        animSourceIndex = step.sourceIndex;
        animDestIndex = step.destinationIndex;
        animMovingPiece = step.movingPiece;
        animCapturedPiece = step.capturedPiece;

        qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - animationStartTimestampMs_;
        float linearProgress = 0.0f;

        if (elapsedMs > step.holdDelayMs && step.durationMs > 0) {
            linearProgress = float(elapsedMs - step.holdDelayMs) / float(step.durationMs);
        }
        
        linearProgress = qBound(0.0f, linearProgress, 1.0f);
        // Cubic ease-out for smooth deceleration: 1 - (1 - t)^3
        easedProgress = 1.0f - qPow(1.0f - linearProgress, 3.0f);
    }

    // 4. Draw stationary pieces
    QFont pieceFont("DejaVu Sans");
    pieceFont.setPixelSize(qMax(12, int(squareSize * 0.63)));
    painter.setFont(pieceFont);

    for (int i = 0; i < 64; ++i) {
        if (i == animSourceIndex) continue;
        if (animDestIndex >= 0 && i == animDestIndex && animCapturedPiece != '.') continue;

        char pieceCode = boardToRender.at(i);
        if (pieceCode == '.') continue;

        QChar glyph = getPieceGlyph(pieceCode);
        QRect squareRect(offsetX + (i % 8) * squareSize, offsetY + (i / 8) * squareSize, squareSize, squareSize);
        bool isWhitePiece = (pieceCode >= 'A' && pieceCode <= 'Z');

        if (isWhitePiece) {
            painter.setPen(QColor("#5a5a5a")); // Drop shadow for white pieces
            painter.drawText(squareRect.translated(1, 1), Qt::AlignCenter, QString(glyph));
            painter.setPen(QColor("#fafafa"));
        } else {
            painter.setPen(QColor("#151515"));
        }
        painter.drawText(squareRect, Qt::AlignCenter, QString(glyph));
    }

    // 5. Draw moving piece in transit
    if (animSourceIndex >= 0 && animDestIndex >= 0 && animMovingPiece != '.') {
        QPointF startCenter(offsetX + (animSourceIndex % 8) * squareSize + squareSize / 2.0, offsetY + (animSourceIndex / 8) * squareSize + squareSize / 2.0);

        QPointF endCenter(offsetX + (animDestIndex % 8) * squareSize + squareSize / 2.0, offsetY + (animDestIndex / 8) * squareSize + squareSize / 2.0);

        QPointF currentCenter = startCenter + (endCenter - startCenter) * easedProgress;

        QRect movingRect(int(currentCenter.x() - squareSize / 2), int(currentCenter.y() - squareSize / 2), squareSize, squareSize);

        QChar glyph = getPieceGlyph(animMovingPiece);

        bool isWhitePiece = (animMovingPiece >= 'A' && animMovingPiece <= 'Z');

        if (isWhitePiece) {
            painter.setPen(QColor("#5a5a5a"));
            painter.drawText(movingRect.translated(1, 1), Qt::AlignCenter, QString(glyph));
            painter.setPen(QColor("#fafafa"));
        } else {
            painter.setPen(QColor("#151515"));
        }
        painter.drawText(movingRect, Qt::AlignCenter, QString(glyph));
    }

    // 6. Draw shrinking captured piece
    if (animDestIndex >= 0 && animCapturedPiece != '.') {
        float shrinkScale = 1.0f - easedProgress;
        int scaledFontSize = qMax(1, int(squareSize * 0.63 * shrinkScale));
        QFont shrinkingFont("DejaVu Sans");
        shrinkingFont.setPixelSize(scaledFontSize);
        painter.setFont(shrinkingFont);

        QRect targetRect(offsetX + (animDestIndex % 8) * squareSize, offsetY + (animDestIndex / 8) * squareSize, squareSize, squareSize);

        QChar glyph = getPieceGlyph(animCapturedPiece);
        bool isWhitePiece = (animCapturedPiece >= 'A' && animCapturedPiece <= 'Z');
        painter.setPen(isWhitePiece ? QColor("#fafafa") : QColor("#151515"));
        painter.drawText(targetRect, Qt::AlignCenter, QString(glyph));
    }

    // Subtle dim overlay while waiting for AI think delay
    if (isAnimating_ && currentAnimationIndex_ < animationSequence_.size()) {
        const AnimationStep &step = animationSequence_[currentAnimationIndex_];
        qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - animationStartTimestampMs_;
        if (elapsedMs < step.holdDelayMs) {
            painter.fillRect(offsetX, offsetY, boardSideLength, boardSideLength, QColor(0, 0, 0, 18));
        }
    }

    // 7. Draw legal move dots and capture rings
    painter.setFont(pieceFont);
    for (int destIndex : legalMoveSquares_) {

        QRect squareRect(offsetX + (destIndex % 8) * squareSize, offsetY + (destIndex / 8) * squareSize, squareSize, squareSize);

        if (boardState_.at(destIndex) == '.') {
            int dotRadius = qMax(6, squareSize / 9);
            painter.setPen(Qt::NoPen);
            painter.setBrush(legalIndicatorColor);
            painter.drawEllipse(squareRect.center(), dotRadius, dotRadius);
        } else {
            QPen ringPen(legalIndicatorColor, qMax(3, squareSize / 18));
            painter.setPen(ringPen);
            painter.setBrush(Qt::NoBrush);
            int ringRadius = squareSize / 2 - 3;
            painter.drawEllipse(squareRect.center(), ringRadius, ringRadius);
        }
    }
}

void BoardWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    if (isAnimating_) cancelAnimation();

    int boardSideLength = qMin(width(), height());
    int squareSize = boardSideLength / 8;
    int offsetX = (width() - boardSideLength) / 2;
    int offsetY = (height() - boardSideLength) / 2;

    int clickX = event->pos().x() - offsetX;
    int clickY = event->pos().y() - offsetY;
    if (clickX < 0 || clickY < 0 || clickX >= boardSideLength || clickY >= boardSideLength) return;

    int col = qBound(0, clickX / squareSize, 7);
    int row = qBound(0, clickY / squareSize, 7);
    emit squareClicked(row * 8 + col);
}

// ---------- MainWindow ------------------------------------------------------

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setupUserInterface();
        handshakeTimer_.setSingleShot(true);
        connect(&handshakeTimer_, &QTimer::timeout, this, &MainWindow::onHandshakeTimeout);
        connect(&serialPort_, &QSerialPort::readyRead, this, &MainWindow::onSerialDataReady);
        connect(&serialPort_, &QSerialPort::errorOccurred, this, &MainWindow::onSerialError);
    }

    void setPortSelectionText(const QString &portName) {
        if (portComboBox_) portComboBox_->setCurrentText(portName);
    }

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onHandshakeTimeout();
    void onNewHumanGameClicked() { startNewGame(false); }
    void onNewAiGameClicked()    { startNewGame(true);  }
    void onResignClicked();
    void onBoardSquareClicked(int squareIndex);
    void onSerialDataReady();
    void onSerialError(QSerialPort::SerialPortError error);
    void refreshAvailablePorts();

private:
    void setupUserInterface();
    void startNewGame(bool isVersusAi);
    void sendSerialCommand(const QString &command);
    void processIncomingSerialLine(const QString &line);
    void updateBoardState(const QString &boardString, const QString &activeTurn,
                          int pendingSourceIndex = -1, int pendingDestIndex = -1);
    void concludeGame(const QString &reasonText);
    void displayCommandRejection(const QString &errorCode);
    void updateConnectionVisuals();
    void updateButtonEnableStates();
    void updateTurnIndicatorLabel();

    void scheduleOutcomePopup(const QString &windowTitle, const QString &headlineText, const QString &detailText);
    void showOutcomePopup(const QString &windowTitle,
                          const QString &headlineText,
                          const QString &detailText);

    // UI Controls
    QComboBox   *portComboBox_        = nullptr;
    QPushButton *refreshPortsButton_  = nullptr;
    QPushButton *connectButton_       = nullptr;
    QPushButton *disconnectButton_    = nullptr;
    QPushButton *newHumanGameButton_  = nullptr;
    QPushButton *newAiGameButton_     = nullptr;
    QPushButton *resignButton_        = nullptr;
    QLabel      *turnIndicatorLabel_  = nullptr;
    QLabel      *statusBarLabel_      = nullptr;
    BoardWidget *chessBoardWidget_    = nullptr;

    // Serial & Handshake State
    QSerialPort serialPort_;
    QByteArray  serialReceiveBuffer_;
    QTimer      handshakeTimer_;
    bool        isMcuVerified_       = false;
    bool        isAwaitingHandshake_ = false;

    // Chess Session State
    QByteArray  currentBoardBytes_   = QByteArray(64, '.');
    QString     activeTurnColor_     = "-";
    bool        isGameOver_          = true;
    int         selectedSquareIndex_ = -1;
    QSet<int>   legalMoveIndices_;
    QSet<int>   lastMoveIndices_;
    int         pendingMoveSource_   = -1;
    int         pendingMoveDest_     = -1;

    // Game-Over Modal State
    int         gameSessionId_       = 0;
    bool        isCurrentModeAi_     = false;
    bool        isOutcomePopupOpen_  = false;
};

void MainWindow::setupUserInterface() {
    auto *centralContainer = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralContainer);

    // Top bar: Serial connection controls
    auto *connectionLayout = new QHBoxLayout();
    connectionLayout->addWidget(new QLabel("Port:", centralContainer));

    portComboBox_ = new QComboBox(centralContainer);
    portComboBox_->setEditable(true);
    portComboBox_->setMinimumWidth(260);
    connectionLayout->addWidget(portComboBox_);

    refreshPortsButton_ = new QPushButton("⟳", centralContainer);
    refreshPortsButton_->setToolTip("Refresh serial ports");
    refreshPortsButton_->setFixedWidth(36);
    connectionLayout->addWidget(refreshPortsButton_);

    connectButton_ = new QPushButton("Connect", centralContainer);
    disconnectButton_ = new QPushButton("Disconnect", centralContainer);
    disconnectButton_->setEnabled(false);
    connectionLayout->addWidget(connectButton_);
    connectionLayout->addWidget(disconnectButton_);
    connectionLayout->addStretch();
    mainLayout->addLayout(connectionLayout);

    // Second bar: Game controls & Turn indicator
    auto *gameControlsLayout = new QHBoxLayout();
    newHumanGameButton_ = new QPushButton("New Game", centralContainer);
    newAiGameButton_    = new QPushButton("New vs AI", centralContainer);
    resignButton_       = new QPushButton("Resign", centralContainer);
    newHumanGameButton_->setEnabled(false);
    newAiGameButton_->setEnabled(false);
    resignButton_->setEnabled(false);

    gameControlsLayout->addWidget(newHumanGameButton_);
    gameControlsLayout->addWidget(newAiGameButton_);
    gameControlsLayout->addWidget(resignButton_);
    gameControlsLayout->addStretch();

    turnIndicatorLabel_ = new QLabel("Turn: —", centralContainer);
    QFont turnFont = turnIndicatorLabel_->font();
    turnFont.setBold(true);
    turnFont.setPointSize(turnFont.pointSize() + 2);
    turnIndicatorLabel_->setFont(turnFont);
    gameControlsLayout->addWidget(turnIndicatorLabel_);
    mainLayout->addLayout(gameControlsLayout);

    // Center: Chessboard
    chessBoardWidget_ = new BoardWidget(centralContainer);
    mainLayout->addWidget(chessBoardWidget_, 1);

    // Bottom: Status bar
    statusBarLabel_ = new QLabel("Not connected", centralContainer);
    mainLayout->addWidget(statusBarLabel_);

    setCentralWidget(centralContainer);
    resize(1200, 950);

    connect(refreshPortsButton_, &QPushButton::clicked, this, &MainWindow::refreshAvailablePorts);
    connect(connectButton_,      &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectButton_,   &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(newHumanGameButton_, &QPushButton::clicked, this, &MainWindow::onNewHumanGameClicked);
    connect(newAiGameButton_,    &QPushButton::clicked, this, &MainWindow::onNewAiGameClicked);
    connect(resignButton_,       &QPushButton::clicked, this, &MainWindow::onResignClicked);
    connect(chessBoardWidget_,   &BoardWidget::squareClicked, this, &MainWindow::onBoardSquareClicked);

    refreshAvailablePorts();
}

void MainWindow::refreshAvailablePorts() {
    QString previousPortText = portComboBox_->currentText();
    portComboBox_->clear();

    const auto availablePorts = QSerialPortInfo::availablePorts();
    int preferredPortIndex = -1;

    for (int i = 0; i < availablePorts.size(); ++i) {
        const QSerialPortInfo &portInfo = availablePorts[i];
        QString displayLabel = portInfo.systemLocation();
        if (!portInfo.description().isEmpty()) {
            displayLabel += QString(" (%1)").arg(portInfo.description());
        }
        portComboBox_->addItem(displayLabel, portInfo.systemLocation());

        // Prefer ST-Link (0x0483) or USB ACM/UART interfaces automatically
        if (portInfo.vendorIdentifier() == 0x0483 ||
            portInfo.description().contains("STLink", Qt::CaseInsensitive) ||
            portInfo.description().contains("ST-Link", Qt::CaseInsensitive) ||
            portInfo.description().contains("UART", Qt::CaseInsensitive) ||
            portInfo.description().contains("USB", Qt::CaseInsensitive) ||
            portInfo.portName().contains("usb", Qt::CaseInsensitive) ||
            portInfo.portName().contains("ACM", Qt::CaseInsensitive)) {
            preferredPortIndex = i;
        }
    }

    if (preferredPortIndex >= 0) {
        portComboBox_->setCurrentIndex(preferredPortIndex);
    } else if (!previousPortText.isEmpty()) {
        portComboBox_->setCurrentText(previousPortText);
    } else if (portComboBox_->count() == 0) {
        portComboBox_->setCurrentText("/dev/ttyACM0");
    }
}

void MainWindow::updateConnectionVisuals() {
    if (serialPort_.isOpen() && isMcuVerified_) {
        connectButton_->setStyleSheet(
            "QPushButton {"
            "  background-color: #4CAF50;"
            "  color: white;"
            "  font-weight: bold;"
            "  border-radius: 4px;"
            "  padding: 4px 10px;"
            "}");
        connectButton_->setText("● Connected");
    } else if (serialPort_.isOpen() && isAwaitingHandshake_) {
        connectButton_->setStyleSheet(
            "QPushButton {"
            "  background-color: #FF9800;"
            "  color: white;"
            "  font-weight: bold;"
            "  border-radius: 4px;"
            "  padding: 4px 10px;"
            "}");
        connectButton_->setText("◌ Connecting…");
    } else {
        connectButton_->setStyleSheet("");
        connectButton_->setText("Connect");
    }
    updateButtonEnableStates();
}

void MainWindow::updateButtonEnableStates() {
    bool isPortOpen = serialPort_.isOpen();
    bool isReadyForCommands = isPortOpen && isMcuVerified_;

    connectButton_->setEnabled(!isPortOpen);
    portComboBox_->setEnabled(!isPortOpen);
    refreshPortsButton_->setEnabled(!isPortOpen);

    // Allow cancelling during handshake or disconnecting once verified
    disconnectButton_->setEnabled(isPortOpen);

    // Game buttons only unlock after the MCU has actually answered over UART
    newHumanGameButton_->setEnabled(isReadyForCommands);
    newAiGameButton_->setEnabled(isReadyForCommands);
    resignButton_->setEnabled(isReadyForCommands && !isGameOver_);
}

void MainWindow::updateTurnIndicatorLabel() {
    if (activeTurnColor_ == "-" || activeTurnColor_.isEmpty()) {
        turnIndicatorLabel_->setText("Turn: —");
    } else {
        turnIndicatorLabel_->setText(
            QString("Turn: %1").arg(activeTurnColor_ == "W" ? "White" : "Black"));
    }
}

void MainWindow::onConnectClicked() {
    QString portPath = portComboBox_->currentData().toString();
    if (portPath.isEmpty() || !portComboBox_->currentText().startsWith(portPath)) {
        portPath = portComboBox_->currentText().section(' ', 0, 0).trimmed();
    }

    serialPort_.setPortName(portPath);
    serialPort_.setBaudRate(115200);
    serialPort_.setDataBits(QSerialPort::Data8);
    serialPort_.setParity(QSerialPort::NoParity);
    serialPort_.setStopBits(QSerialPort::OneStop);
    serialPort_.setFlowControl(QSerialPort::NoFlowControl);

    if (!serialPort_.open(QIODevice::ReadWrite)) {
        statusBarLabel_->setText(
            QString("Cannot open %1: %2").arg(portPath, serialPort_.errorString()));
        return;
    }

    // Clear stale OS buffers and require an active response from the MCU before turning green
    serialPort_.clear();
    serialReceiveBuffer_.clear();
    isMcuVerified_ = false;
    isAwaitingHandshake_ = true;

    updateConnectionVisuals();
    statusBarLabel_->setText(QString("Opened %1 — waiting for microcontroller response…").arg(portPath));

    handshakeTimer_.start(HANDSHAKE_TIMEOUT_MS);
    sendSerialCommand("CONNECT");
}

void MainWindow::onHandshakeTimeout() {
    if (!isAwaitingHandshake_ || isMcuVerified_) return;

    // Port opened (e.g. ST-Link USB bridge powered), but the MCU never replied
    isAwaitingHandshake_ = false;
    serialPort_.close();

    updateConnectionVisuals();
    statusBarLabel_->setText(
        "Connection failed — no response from microcontroller (check power/firmware).");
}

void MainWindow::onDisconnectClicked() {
    handshakeTimer_.stop();
    if (serialPort_.isOpen()) {
        if (isMcuVerified_) {
            sendSerialCommand("DISCONNECT");
            serialPort_.flush();
            serialPort_.waitForBytesWritten(100);
        }
        serialPort_.close();
    }

    isMcuVerified_ = false;
    isAwaitingHandshake_ = false;
    currentBoardBytes_ = QByteArray(64, '.');
    activeTurnColor_ = "-";
    isGameOver_ = true;
    selectedSquareIndex_ = -1;
    legalMoveIndices_.clear();
    lastMoveIndices_.clear();
    pendingMoveSource_ = pendingMoveDest_ = -1;

    chessBoardWidget_->setBoardState(currentBoardBytes_);
    chessBoardWidget_->setSelectedSquare(-1);
    chessBoardWidget_->setLegalDestinationSquares({});
    chessBoardWidget_->setHighlightedMoveSquares({});

    updateTurnIndicatorLabel();
    updateConnectionVisuals();
    statusBarLabel_->setText("Disconnected");
}

void MainWindow::startNewGame(bool isVersusAi) {
    if (!serialPort_.isOpen() || !isMcuVerified_) return;

    gameSessionId_++;
    isCurrentModeAi_ = isVersusAi;
    isGameOver_ = true;
    selectedSquareIndex_ = -1;
    legalMoveIndices_.clear();
    lastMoveIndices_.clear();
    pendingMoveSource_ = pendingMoveDest_ = -1;

    chessBoardWidget_->setSelectedSquare(-1);
    chessBoardWidget_->setLegalDestinationSquares({});
    chessBoardWidget_->setHighlightedMoveSquares({});

    statusBarLabel_->setText(isVersusAi ? "Starting new game vs AI…" : "Starting new game…");

    // Send only the NEW command (sending DISCONNECT first drops the session / overflows RX)
    sendSerialCommand(isVersusAi ? "NEW:AI" : "NEW");
}

void MainWindow::onResignClicked() {
    if (isGameOver_) return;
    sendSerialCommand("RESIGN");
    isGameOver_ = true;
    selectedSquareIndex_ = -1;
    legalMoveIndices_.clear();
    chessBoardWidget_->setSelectedSquare(-1);
    chessBoardWidget_->setLegalDestinationSquares({});
    updateButtonEnableStates();
}

void MainWindow::onBoardSquareClicked(int squareIndex) {
    if (isGameOver_ || !serialPort_.isOpen() || !isMcuVerified_) return;

    // 1st click: Select piece and query MCU for legal moves
    if (selectedSquareIndex_ < 0) {
        selectedSquareIndex_ = squareIndex;
        legalMoveIndices_.clear();
        chessBoardWidget_->setSelectedSquare(squareIndex);
        chessBoardWidget_->setLegalDestinationSquares({});
        sendSerialCommand(QString("MOVES:%1").arg(indexToSquareString(squareIndex)));
        return;
    }

    // Clicking the same square deselects it
    if (squareIndex == selectedSquareIndex_) {
        selectedSquareIndex_ = -1;
        legalMoveIndices_.clear();
        chessBoardWidget_->setSelectedSquare(-1);
        chessBoardWidget_->setLegalDestinationSquares({});
        return;
    }

    // 2nd click: Attempt move from selected square to clicked destination
    int sourceSquare = selectedSquareIndex_;
    selectedSquareIndex_ = -1;
    legalMoveIndices_.clear();
    chessBoardWidget_->setSelectedSquare(-1);
    chessBoardWidget_->setLegalDestinationSquares({});

    pendingMoveSource_ = sourceSquare;
    pendingMoveDest_ = squareIndex;
    sendSerialCommand(QString("MOVE:%1%2").arg(indexToSquareString(sourceSquare), indexToSquareString(squareIndex)));
}

void MainWindow::sendSerialCommand(const QString &command) {
    if (!serialPort_.isOpen()) return;
    serialPort_.write((command + "\n").toUtf8());
    serialPort_.flush();
}

void MainWindow::onSerialDataReady() {
    serialReceiveBuffer_ += serialPort_.readAll();
    int newlineIndex;
    while ((newlineIndex = serialReceiveBuffer_.indexOf('\n')) >= 0) {
        QByteArray rawLine = serialReceiveBuffer_.left(newlineIndex);
        serialReceiveBuffer_.remove(0, newlineIndex + 1);
        QString cleanLine = QString::fromLatin1(rawLine).trimmed();
        if (!cleanLine.isEmpty()) {
            processIncomingSerialLine(cleanLine);
        }
    }
}

void MainWindow::onSerialError(QSerialPort::SerialPortError error) {
    if (error == QSerialPort::NoError) return;

    // Handle physical cable unplug or device loss
    if (error == QSerialPort::ResourceError ||
        error == QSerialPort::DeviceNotFoundError ||
        error == QSerialPort::PermissionError) {
        QString errorText = serialPort_.errorString();
        onDisconnectClicked();
        statusBarLabel_->setText(QString("Serial disconnected: %1").arg(errorText));
    }
}

void MainWindow::processIncomingSerialLine(const QString &line) {
    // Any valid protocol frame proves the MCU is powered and running the firmware
    if (isAwaitingHandshake_) {
        handshakeTimer_.stop();
        isAwaitingHandshake_ = false;
        isMcuVerified_ = true;
        updateConnectionVisuals();
        statusBarLabel_->setText("Connected — press New Game, New vs AI, or a physical board button.");
    }

    if (line.startsWith("EVENT:")) {
        QString eventType = line.mid(6);
        if (eventType == "NEW") {
            gameSessionId_++;
            isCurrentModeAi_ = false;
            statusBarLabel_->setText("Board button: New game (Human vs Human)");
        } else if (eventType == "NEW_AI") {
            gameSessionId_++;
            isCurrentModeAi_ = true;
            statusBarLabel_->setText("Board button: New game vs AI");
        }
        return;
    }

    if (line.startsWith("OK:BOARD:") || line.startsWith("BOARD:")) {
        int prefixLength = line.startsWith("OK:BOARD:") ? 9 : 6;
        QStringList payloadParts = line.mid(prefixLength).split(":");

        if (payloadParts.size() >= 3 && payloadParts[0].size() == 64) {
            bool wasBoardReset = (payloadParts[0] == QString(INITIAL_BOARD_STATE) && currentBoardBytes_ != QByteArray(64, '.') && currentBoardBytes_ != QByteArray(INITIAL_BOARD_STATE));
            updateBoardState(payloadParts[0], payloadParts[2], pendingMoveSource_, pendingMoveDest_);
            pendingMoveSource_ = pendingMoveDest_ = -1;

            if (wasBoardReset) {
                statusBarLabel_->setText("New game started — White to move");
            }
        }

        return;
    }

    if (line.startsWith("WIN:")) {
        QStringList parts = line.split(":");
        QString winnerCode = parts.size() > 1 ? parts[1] : "?";
        int boardTagIndex = parts.indexOf("BOARD");

        if (boardTagIndex >= 0 && boardTagIndex + 1 < parts.size()) {
            updateBoardState(parts[boardTagIndex + 1], "-", pendingMoveSource_, pendingMoveDest_);
        }

        pendingMoveSource_ = pendingMoveDest_ = -1;

        QString winnerName = (winnerCode == "W") ? "White" : "Black";
        QString loserName  = (winnerCode == "W") ? "Black" : "White";

        concludeGame(QString("%1 wins by checkmate").arg(winnerName));
        scheduleOutcomePopup("Checkmate", QString("%1 wins by checkmate").arg(winnerName), QString("%1 loses").arg(loserName));

        return;
    }

    if (line.startsWith("DRAW:")) {
        QStringList parts = line.split(":");
        int boardTagIndex = parts.indexOf("BOARD");

        if (boardTagIndex >= 0 && boardTagIndex + 1 < parts.size()) {
            updateBoardState(parts[boardTagIndex + 1], "-", pendingMoveSource_, pendingMoveDest_);
        }

        pendingMoveSource_ = pendingMoveDest_ = -1;

        concludeGame("Draw (stalemate / 50-move / insufficient material)");
        scheduleOutcomePopup("Draw", "The game ended in a draw", "Reason: stalemate, 50-move rule, or insufficient material");

        return;
    }

    if (line.startsWith("OK:GAMEOVER:")) {
        QString winnerCode = line.split(":").last();
        pendingMoveSource_ = pendingMoveDest_ = -1;

        QString winnerName = (winnerCode == "W") ? "White" : "Black";
        QString loserName  = (winnerCode == "W") ? "Black" : "White";

        concludeGame(QString("%1 wins by resignation").arg(winnerName));
        scheduleOutcomePopup("Resignation", QString("%1 wins by resignation").arg(winnerName), QString("%1 resigned").arg(loserName));

        return;
    }

    if (line.startsWith("VALID:")) {
        if (selectedSquareIndex_ >= 0) {
            QString moveListPayload = line.mid(6);
            legalMoveIndices_.clear();

            if (!moveListPayload.isEmpty()) {
                for (const QString &squareStr : moveListPayload.split(",")) {
                    int destIndex = squareStringToIndex(squareStr);
                    if (destIndex >= 0) legalMoveIndices_.insert(destIndex);
                }
            }

            chessBoardWidget_->setLegalDestinationSquares(legalMoveIndices_);
        }
        return;
    }

    if (line == "OK") {
        statusBarLabel_->setText("Connected — Ready");
        return;
    }

    if (line.startsWith("NACK:")) {
        pendingMoveSource_ = pendingMoveDest_ = -1;
        displayCommandRejection(line.mid(5));
        return;
    }

    statusBarLabel_->setText(QString("Unhandled message: %1").arg(line));
}

void MainWindow::updateBoardState(const QString &boardString, const QString &activeTurn, int pendingSourceIndex, int pendingDestIndex) {
    QByteArray nextBoardBytes = boardString.toLatin1();

    if (nextBoardBytes.size() != 64) return;

    // Highlight squares that changed during the last move
    lastMoveIndices_.clear();
    if (currentBoardBytes_ != QByteArray(64, '.') &&
        nextBoardBytes != QByteArray(INITIAL_BOARD_STATE)) {
        for (int i = 0; i < 64; ++i) {
            if (currentBoardBytes_.at(i) != nextBoardBytes.at(i)) {
                lastMoveIndices_.insert(i);
            }
        }
    }

    QByteArray previousBoardBytes = currentBoardBytes_;
    currentBoardBytes_ = nextBoardBytes;

    if (activeTurn == "W" || activeTurn == "B") {
        activeTurnColor_ = activeTurn;
    }

    isGameOver_ = false;

    // Snap immediately on initial load or full reset; otherwise animate the transition
    bool isInitialOrReset = (previousBoardBytes == QByteArray(64, '.') || nextBoardBytes == QByteArray(INITIAL_BOARD_STATE) || previousBoardBytes == nextBoardBytes);

    if (isInitialOrReset) {
        chessBoardWidget_->setBoardState(currentBoardBytes_);
    } else {
        chessBoardWidget_->transitionToBoard(currentBoardBytes_, pendingSourceIndex, pendingDestIndex);
    }

    chessBoardWidget_->setHighlightedMoveSquares(lastMoveIndices_);
    chessBoardWidget_->setSelectedSquare(-1);
    chessBoardWidget_->setLegalDestinationSquares({});

    updateTurnIndicatorLabel();
    updateButtonEnableStates();
    statusBarLabel_->setText(QString("%1 to move").arg(activeTurnColor_ == "W" ? "White" : "Black"));
}

void MainWindow::concludeGame(const QString &reasonText) {
    isGameOver_ = true;
    selectedSquareIndex_ = -1;
    legalMoveIndices_.clear();
    chessBoardWidget_->setSelectedSquare(-1);
    chessBoardWidget_->setLegalDestinationSquares({});
    updateButtonEnableStates();
    statusBarLabel_->setText(QString("Game over — %1").arg(reasonText));
}

void MainWindow::displayCommandRejection(const QString &errorCode) {
    QString explanation;
    if (errorCode == "NOGAME") explanation = "No game in progress — press New Game.";
    else if (errorCode == "RANGE") explanation = "Move out of range — invalid square or format.";
    else if (errorCode == "EMPTY") explanation = "Source square is empty.";
    else if (errorCode == "NOTURN") explanation = "That piece is not yours to move.";
    else if (errorCode == "OCCUPIED") explanation = "Destination square is occupied by your piece.";
    else if (errorCode == "ILLEGAL") explanation = "Illegal move for that piece (or leaves king in check).";
    else if (errorCode == "BADCMD") explanation = "Malformed or unknown command.";
    else if (errorCode == "TIMEOUT") explanation = "Incomplete frame timed out.";
    else                              explanation = errorCode;

    statusBarLabel_->setText(QString("Rejected — %1").arg(explanation));
}

// ---------- Game-Over Outcome Modal -----------------------------------------

void MainWindow::scheduleOutcomePopup(const QString &windowTitle, const QString &headlineText, const QString &detailText) {
    int capturedSessionId = gameSessionId_;
    QTimer::singleShot(OUTCOME_POPUP_DELAY_MS, this, [this, capturedSessionId, windowTitle, headlineText, detailText]() {
        // Suppress popup if the user already started a new game in the meantime
        if (gameSessionId_ != capturedSessionId) return;
        showOutcomePopup(windowTitle, headlineText, detailText);
    });
}

void MainWindow::showOutcomePopup(const QString &windowTitle, const QString &headlineText, const QString &detailText) {
    if (isOutcomePopupOpen_) return;

    isOutcomePopupOpen_ = true;
    QMessageBox messageBox(this);
    messageBox.setWindowTitle(windowTitle);
    messageBox.setIcon(QMessageBox::Information);
    messageBox.setText(headlineText);

    if (!detailText.isEmpty()) {
        messageBox.setInformativeText(detailText);
    }

    QPushButton *playAgainButton = messageBox.addButton("Play Again", QMessageBox::AcceptRole);
    messageBox.addButton("Close", QMessageBox::RejectRole);
    messageBox.setDefaultButton(playAgainButton);

    messageBox.exec();
    isOutcomePopupOpen_ = false;

    if (messageBox.clickedButton() == playAgainButton) {
        startNewGame(isCurrentModeAi_);
    }
}

// ---------- Entry Point -----------------------------------------------------

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    MainWindow mainWindow;
    mainWindow.show();
    if (argc > 1) {
        mainWindow.setPortSelectionText(QString::fromLocal8Bit(argv[1]));
    }
    return app.exec();
}

#include "main.moc"