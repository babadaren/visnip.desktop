#pragma once

#include "core/AnnotationModel.h"
#include "core/AppConfig.h"
#include "core/LocalTranslation.h"
#include "core/LongCaptureMatch.h"
#include "core/TranslationCompositor.h"

#include <QColor>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QFuture>
#include <QImage>
#include <QLabel>
#include <QPoint>
#include <QPointer>
#include <QQueue>
#include <QRect>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QTimer>
#include <QWidget>
#include <QWheelEvent>
#include <atomic>
#include <memory>

class QCloseEvent;
class QContextMenuEvent;
class QEvent;
class QHideEvent;
class QShowEvent;
template <typename T>
class QFutureWatcher;

namespace Visnip::Ui {
class CompactToolbar;
}

namespace Visnip {

class AppConfig;
class TranslationReviewPanel;
class ImageTranslationService;
class LongCaptureViewportLayer;
class LongCaptureFrameGrabber;
class LocalTextTranslationService;
class OcrService;
class TextTranslationService;
struct LongCaptureFrameGrabResult;

QImage buildFastTranslateGlassImage(const QImage& source);
bool fastTranslationPlacementIsCompatible(const QRect& sourceRect,
                                          const QRect& destinationRect);
QString captureCursorColorDisplayText(const QColor& color);
QImage buildCaptureMagnifierPanelImage(const QImage& sourceImage,
                                       const QPoint& localMouse,
                                       const QPoint& globalMouse,
                                       const QColor& accent,
                                       bool showCursorColor);

class CaptureOverlayWindow : public QWidget {
    Q_OBJECT
public:
    explicit CaptureOverlayWindow(AppConfig* config,
                                  OcrService* ocrService = nullptr,
                                  ImageTranslationService* imageTranslationService = nullptr,
                                  QWidget* parent = nullptr);
    ~CaptureOverlayWindow() override;

    static void warmupCaptureBackend();
    // Starts (or returns) the background font-database population; run this
    // before warmupCaptureBackend() so widget warmups do not block the GUI
    // thread on the first font enumeration.
    static QFuture<QStringList> warmupFontCatalog();
    void beginCapture(const QRect& initialSelection = QRect());
    QImage renderResult() const;
    QRect selectionRect() const { return selection_; }
signals:
    void copyRequested(QImage image);
    void saveRequested(QImage image);
    void pinRequested(QImage image, QRect sourceRect);
    // Global logical selection for the question panel, with the selection's
    // pixels as captured (no annotations) for its preview.
    void questionRequested(QRect globalSelection, QImage preview);
    void cancelled();
    void finished(QRect selection);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void onToolSelected(const QString& id);
    void onToolbarAction(const QString& id);

private:
    enum class LongCaptureFrameResult {
        NoChange,
        Accepted,
        Rejected,
        Deferred,
        Failed
    };

    enum class LongCaptureViewportRefreshPolicy {
        Deferred,
        CommitBeforeReveal,
    };

    enum class LongCaptureInputRegionClearPolicy {
        Deferred,
        CommitBeforeReveal,
    };

    enum class LongCaptureFrameResume {
        Synchronous,
        Progress,
        AnnotationConfirm,
    };

    struct LongCaptureWheelSegment {
        qint64 delta = 0;
        QPoint globalPos;
        quint16 keyState = 0;
    };

    enum class ShapeKind {
        Rectangle,
        Ellipse
    };

    enum class ShapePaintMode {
        StrokeOnly,
        Filled
    };

    struct ShapeToolState {
        ShapeKind kind = ShapeKind::Rectangle;
        ShapePaintMode mode = ShapePaintMode::StrokeOnly;
        int strokeWidth = 2;
        QColor strokeColor = QColor(239, 68, 68);
        QColor fillColor = QColor(239, 68, 68);
        int fillAlpha = 48;
    };

    struct ArrowToolState {
        int strokeWidth = 2;
        QColor strokeColor = QColor(239, 68, 68);
        ArrowHeadMode headMode = ArrowHeadMode::SingleArrow;
    };

    struct MosaicToolState {
        MosaicPaintMode paintMode = MosaicPaintMode::Fill;
        MosaicEffectMode effectMode = MosaicEffectMode::GaussianBlur;
        int strength = 15;
    };

    struct EraserToolState {
        MosaicPaintMode paintMode = MosaicPaintMode::Brush;
        int size = 18;
    };

    enum class EraserRectDragMode {
        None,
        Move,
        ResizeTopLeft,
        ResizeTopRight,
        ResizeBottomLeft,
        ResizeBottomRight
    };

    enum class SelectionDragMode {
        None,
        Move,
        ResizeLeft,
        ResizeRight,
        ResizeTop,
        ResizeBottom,
        ResizeTopLeft,
        ResizeTopRight,
        ResizeBottomLeft,
        ResizeBottomRight
    };

    enum class ShapeEditDragMode {
        None,
        Move,
        ResizeLeft,
        ResizeRight,
        ResizeTop,
        ResizeBottom,
        ResizeTopLeft,
        ResizeTopRight,
        ResizeBottomLeft,
        ResizeBottomRight
    };

    enum class ArrowEditDragMode {
        None,
        Move,
        ResizeStart,
        ResizeEnd
    };

    enum class Mode {
        Idle,
        Selecting,
        Ready,
        DrawingAnnotation,
        MovingSelection,
        ResizingSelection,
        EditingShape,
        EditingArrow
    };

    enum class RightButtonAction {
        None,
        ResetSelection,
        Close
    };

    struct ActiveShapeEditState {
        bool active = false;
        int index = -1;
        ShapeEditDragMode dragMode = ShapeEditDragMode::None;
        QPoint dragStart;
        QRectF dragStartRect;
    };

    struct ActiveArrowEditState {
        bool active = false;
        int index = -1;
        ArrowEditDragMode dragMode = ArrowEditDragMode::None;
        QPoint dragStart;
        QVector<QPointF> dragStartPoints;
    };

    void captureDesktop();
    QRect normalizedFromPoints(const QPoint& a, const QPoint& b) const;
    void finalizeSelection();
    void beginRightButtonAction();
    void finishRightButtonAction();
    void resetSelectionFromRightButton();
    void closeFromRightButton();
    void installRightButtonEventFilter(QWidget* widget);
    void placeToolbar();
    void hideToolbar();
    void rebuildOptionsBar();
    void placeOptionsBar();
    void hideOptionsBar();
    void requestTranslation();
    // Either a complete cloud-translated image or the local OCR + text
    // translation result is shown in place of the selection.
    void toggleFastTranslation();
    void startFastTranslation();
    void prepareFastTranslationInputAndLaunch(quint64 serial);
    void queueFastTranslateGlass(quint64 serial);
    void startFastTranslateGlassJob(QImage source, QRect sourceRect,
                                    quint64 serial);
    void beginFastTranslationWork(bool cloudImage);
    void cancelFastTranslation();
    void failFastTranslation(const QString& message);
    void setFastTranslateVisible(bool visible);
    void stopFastTranslateProgress();
    bool fastTranslateShown() const;
    void ensureFastTranslateServices(bool cloudImage);
    // Composes per-unit translations from the online text service or the lite
    // offline engine; unresolvedRegions kept their source after validation.
    void finishFastTextTranslation(const QStringList& translations,
                                   qint64 elapsedMs,
                                   int unresolvedRegions);
    bool fastTranslationConfigurationMatchesPending() const;
    void moveFastTranslationPresentation(const QRect& previousSelection);
    void updateFastTranslateToolbarState();
    void drawFastTranslateOverlay(QPainter& painter);
    void refreshOptionsBarVisuals();
    void syncCursorForTool();
    void applyShapeKind(ShapeKind kind);
    void applyShapePaintMode(ShapePaintMode mode);
    void applyArrowHeadMode(ArrowHeadMode mode);
    void applyMosaicPaintMode(MosaicPaintMode mode);
    void applyMosaicEffectMode(MosaicEffectMode mode);
    void applyMosaicStrength(int value);
    void applyMosaicStateToActiveRegion();
    void applyEraserPaintMode(MosaicPaintMode mode);
    void applyEraserSize(int value);
    void applyTextBold(bool enabled);
    void applyTextItalic(bool enabled);
    void applyTextOutline(bool enabled);
    void applyTextFontFamily(const QString& family);
    void applyTextFontSize(int value);
    void beginInlineTextEdit(const QPoint& pos);
    void commitInlineTextEdit();
    void cancelInlineTextEdit();
    void applyTextStyleToInlineEditor();
    void resizeInlineTextEditorToDocument();
    QColor currentShapeColor() const;
    void applyToolColor(const QColor& color);
    void applyToolWidth(int value);
    void applyPersistedStyleForTool();
    void persistToolStyles();
    void renderAnnotationsToImage(QImage& image, const QImage& base, bool includeCurrent, const QPointF& offset = QPointF()) const;
    // Returns the selection composited with every committed annotation,
    // rebuilding it only when the selection rect, the desktop frame or the
    // annotation document actually changed. paintEvent used to redo two
    // full-selection deep copies plus a complete annotation replay on every
    // frame, which no amount of update(rect) could avoid because that work
    // happens outside the painter's clip.
    const QImage& committedSelectionComposite() const;
    void invalidateSelectionComposite();
    // Bounding box of the pointer-anchored overlays (magnifier panel, cursor
    // colour chip, floating size label). Null when none of them is visible.
    QRect pointerOverlayDirtyRect(const QPoint& pointer) const;
    // Selection rect grown to cover its border, handles and size label.
    QRect selectionDirtyRect(const QRect& selection) const;
    void renderAnnotationDocumentToImage(QImage& image,
                                         const QImage& base,
                                         const AnnotationDocument& document,
                                         bool includeCurrent,
                                         const QPointF& offset = QPointF()) const;
    void drawSelection(QPainter& painter) const;
    void drawLabels(QPainter& painter) const;
    void drawMagnifier(QPainter& painter) const;
    void drawEraserControlOverlay(QPainter& painter) const;
    void drawActiveShapeEditOverlay(QPainter& painter) const;
    void drawActiveArrowEditOverlay(QPainter& painter) const;
    void drawLongCapturePreview(QPainter& painter) const;
    void startAnnotation(const QPoint& pos);
    void updateAnnotation(const QPoint& pos);
    void finishAnnotation(const QPoint& pos);
    AnnotationType typeFromToolId(const QString& id) const;
    bool canStartAnnotation(const QPoint& pos) const;
    void requestCopy();
    void requestSave();
    void requestPin();
    void requestQuestion();
    void finalizeLongCaptureOutput();
    QImage prepareOutputResult();
    SelectionDragMode selectionDragModeAt(const QPoint& pos) const;
    QRect resizedSelectionRect(const QPoint& pos) const;
    void updateSelectionHoverCursor(const QPoint& pos);
    bool isActiveShapeEditable() const;
    void clearActiveShapeEdit();
    void activateShapeEditAt(int index);
    void activateLatestShapeIfEditable();
    void syncShapeStateFromActiveShape();
    void applyShapeStateToActiveShape();
    ShapeEditDragMode shapeEditDragModeAt(const QPoint& pos) const;
    QRectF resizedActiveShapeRect(const QPoint& pos) const;
    void updateShapeEditHoverCursor(const QPoint& pos);
    void beginShapeEditDrag(ShapeEditDragMode mode, const QPoint& pos);
    void updateShapeEditDrag(const QPoint& pos);
    void finishShapeEditDrag();
    bool isActiveArrowEditable() const;
    void clearActiveArrowEdit();
    void activateArrowEditAt(int index);
    void activateLatestArrowIfEditable();
    void syncArrowStateFromActiveArrow();
    void applyArrowStateToActiveArrow();
    ArrowEditDragMode arrowEditDragModeAt(const QPoint& pos) const;
    void updateArrowEditHoverCursor(const QPoint& pos);
    void beginArrowEditDrag(ArrowEditDragMode mode, const QPoint& pos);
    void updateArrowEditDrag(const QPoint& pos);
    void finishArrowEditDrag();
    void commitPendingEraserFill();
    void cancelPendingEraserFill();
    EraserRectDragMode eraserRectDragModeAt(const QPoint& pos) const;
    QRectF clampedEraserRect(QRectF rect) const;
    AnnotationDocument& activeAnnotationDocument();
    const AnnotationDocument& activeAnnotationDocument() const;
    QRectF activeAnnotationBounds() const;
    void updateAnnotationViews();
    bool isLongCaptureToolActive() const;
    bool isLongCaptureBrowseMode() const;
    bool isLongCaptureAnnotationMode() const;
    void enterLongCaptureMode();
    void leaveLongCaptureMode();
    void abortLongCaptureMode(const QString& reason);
    void requestLongCaptureAnnotationMode(const QString& toolId);
    void cancelPendingLongCaptureAnnotationMode();
    void scheduleLongCaptureAnnotationConfirmation(int minimumDelayMs = 0);
    void tryEnterLongCaptureAnnotationMode();
    void enterLongCaptureAnnotationMode(const QString& toolId);
    void leaveLongCaptureAnnotationMode(bool cancelUnfinished = false);
    void makeLongCaptureAnnotationsOverlayRelative();
    void makeLongCaptureAnnotationsDocumentRelative();
    void createLongCaptureLayers();
    void destroyLongCaptureLayers();
    void syncLongCaptureLayers();
    bool refreshLongCaptureViewportLayer(
        LongCaptureViewportRefreshPolicy policy = LongCaptureViewportRefreshPolicy::Deferred);
    bool hasLongCaptureResizeFrontier(SelectionDragMode mode) const;
    bool canResizeLongCaptureEdge(SelectionDragMode mode) const;
    void prepareLongCaptureResizePreview();
    void finishLongCaptureViewportResize(const QRect& previousSelection);
    bool resetLongCaptureFrames(const QString& reason);
    bool repairLongCaptureCommittedFrame(const QImage& image,
                                         const LongCapture::RowSignature& signature,
                                         int documentY);
    void captureLongFrameDuringScroll();
    LongCaptureFrameResult processLongCaptureFrame(
        LongCaptureFrameResume resume = LongCaptureFrameResume::Synchronous);
    bool canCaptureLongFrameAsync() const;
    bool requestLongCaptureFrameAsync(LongCaptureFrameResume resume);
    void handleLongCaptureFrameGrabbed(LongCaptureFrameGrabResult result);
    void cancelLongCaptureFrameRequest();
    QImage captureLongFrameImage(const QRect& logicalRect = QRect());
    bool appendLongCaptureContent(const QImage& image, const LongCapture::RowSignature& signature, int documentY);
    bool ensureLongCaptureCanvasRange(int topY, int bottomY);
    QImage longCaptureResultImage() const;
    QRect longCapturePreviewPanelRect() const;
    void scheduleLongCapturePreviewRefresh();
    void refreshLongCapturePreviewFit();
    bool updateLongCapturePreviewMip(int outputTop, int outputBottom, int targetHeight);
    void invalidateLongCapturePreviewMip();
    bool sendLongCaptureWheel(int wheelDelta, const QPoint& globalPos, quint16 keyState);
    void enqueueLongCaptureWheel(int wheelDelta, const QPoint& globalPos, quint16 keyState = 0);
    void dispatchNextLongCaptureWheel();
    void completeLongCaptureWheelDispatch(bool moved);
    void abortLongCaptureWheelDispatch(const QString& reason);
    void resolveLongCaptureTarget(const QPoint& globalPos);
    void installLongCaptureWheelHook();
    void uninstallLongCaptureWheelHook();
    bool handleLongCaptureNativeWheel(int wheelDelta, const QPoint& globalPos, quint16 keyState = 0);
    void applyLongCaptureInputRegion();
    void clearLongCaptureInputRegion(
        LongCaptureInputRegionClearPolicy policy = LongCaptureInputRegionClearPolicy::Deferred);
    bool applyCaptureExclusion();
    bool shouldShowPointerInfo() const;
    QRect pointerRect() const;
    QRect adjustedFloatingRect(QRect floating, const QSize& margin = QSize(8, 8)) const;

    AppConfig* config_ = nullptr;
    CaptureSettings captureSettings_;
    UiSettings uiSettings_;
    ToolStyleSettings toolStyles_;
    bool copyThenExit_ = true;
    int overlayId_ = 0;
    QImage desktopImage_;
    QRect virtualGeometry_;
    QRect selection_;
    QPoint dragStart_;
    QPoint lastMouse_;
    QPoint moveStartSelectionTopLeft_;
    SelectionDragMode selectionDragMode_ = SelectionDragMode::None;
    QRect selectionDragStartRect_;
    qint64 beginCaptureStartedAtMs_ = -1;
    bool firstPaintLogged_ = false;
    Mode mode_ = Mode::Idle;
    QString activeTool_;
    ShapeToolState shapeState_;
    ArrowToolState arrowState_;
    MosaicToolState mosaicState_;
    EraserToolState eraserState_;
    AnnotationStyle currentStyle_;
    AnnotationDocument annotations_;
    AnnotationDocument longCaptureAnnotations_;
    AnnotationItem currentAnnotation_;
    bool hasCurrentAnnotation_ = false;
    // Cache for committedSelectionComposite(). Mutable because paintEvent is
    // const; the cache is pure derived state.
    mutable QImage selectionCompositeBase_;      // raw selection pixels, ARGB32
    mutable QImage selectionCompositeCommitted_; // base + committed annotations
    mutable QRect selectionCompositeRect_;
    mutable qint64 selectionCompositeDesktopKey_ = -1;
    mutable quint64 selectionCompositeRevision_ = 0;
    mutable bool selectionCompositeValid_ = false;
    mutable quint64 selectionCompositeTranslateEpoch_ = 0;
    mutable QImage inProgressComposite_;         // scratch reused across frames

    // --- Fast in-place translation state ---
    QPointer<OcrService> fastOcr_;
    bool fastOcrConnected_ = false;
    ImageTranslationService* fastImage_ = nullptr;
    bool fastImageConnected_ = false;
    TextTranslationService* fastText_ = nullptr;
    LocalTextTranslationService* fastLocalText_ = nullptr;
    LocalMt::ParagraphPlan fastParagraphPlan_; // lite offline request entries
    bool fastPendingLiteOffline_ = false;
    QString fastPendingOfflineRoot_;
    bool fastTranslateRunning_ = false;
    bool fastTranslateVisible_ = false;
    QImage fastTranslatedImage_;      // selection-sized composited result
    QRect fastTranslatedRect_;        // current destination of the cached result
    QString fastTranslatedLanguage_;
    QString fastTranslatedProviderKey_;
    QString fastTranslatedOcrPackKey_;
    QRect fastPendingRect_;           // immutable source selection captured at start
    QString fastPendingLanguage_;
    QString fastPendingProviderKey_;
    QString fastPendingOcrPackId_;
    QString fastPendingOcrPackKey_;
    bool fastPendingCloudImage_ = false;
    QImage fastPendingImage_;          // one crop reused by OCR, grouping and compose
    QVector<Translate::TextBlock> fastTranslationUnits_;
    QString fastDiagnosticsDir_;      // per-request original/result/OCR evidence
    QString fastTranslateStage_;      // stage recorded if the request fails
    quint64 fastJobSerial_ = 0;       // guards stale async results
    quint64 fastWorkLaunchAwaitingPaintSerial_ = 0;
    quint64 fastProgressPaintSerial_ = 0;
    quint64 fastOcrRevision_ = 0;     // request owned by this overlay
    quint64 fastTranslateEpoch_ = 0;  // bumps whenever the shown base changes
    QString fastTranslateStatus_;     // progress/error badge text
    QPointer<TranslationReviewPanel> fastReviewPanel_;
    QElapsedTimer fastJobTimer_;
    QTimer* fastSpinnerTimer_ = nullptr; // repaints the in-progress spinner
    QImage fastGlassImage_;           // selection-sized frosted loading mask
    QFutureWatcher<QImage>* fastGlassWatcher_ = nullptr;
    std::shared_ptr<std::atomic_bool> fastGlassCancelFlag_;
    QImage fastGlassQueuedSource_;
    QRect fastGlassQueuedRect_;
    quint64 fastGlassQueuedSerial_ = 0;
    bool eraserFillPending_ = false;
    RightButtonAction rightButtonAction_ = RightButtonAction::None;
    ActiveShapeEditState activeShapeEdit_;
    ActiveArrowEditState activeArrowEdit_;
    EraserRectDragMode eraserRectDragMode_ = EraserRectDragMode::None;
    QPoint eraserRectDragStart_;
    QRectF eraserRectDragStartRect_;
    QPointer<Ui::CompactToolbar> toolbar_;
    QPointer<QFrame> optionsBar_;
    QPointer<QFrame> standardOptionsBar_;
    QPointer<QFrame> textOptionsBar_;
    QPointer<QToolButton> optionStrokePreviewButton_;
    QPointer<QFrame> optionCurrentColorButton_;
    QPointer<QSlider> optionMosaicStrengthSlider_;
    QPointer<QLabel> optionMosaicStrengthLabel_;
    QPointer<QToolButton> optionMosaicEffectButton_;
    QPointer<QToolButton> optionTextBoldButton_;
    QPointer<QToolButton> optionTextItalicButton_;
    QPointer<QToolButton> optionTextOutlineButton_;
    QPointer<QFrame> optionTextCurrentColorButton_;
    QPointer<QComboBox> optionTextFontCombo_;
    QPointer<QComboBox> optionTextSizeCombo_;
    QPointer<QWidget> inlineTextBox_;
    QTimer longCaptureProgressTimer_;
    QTimer longCaptureAnnotationModeTimer_;
    QTimer longCapturePreviewRefreshTimer_;
    QPointer<LongCaptureViewportLayer> longCaptureViewportLayer_;
    QImage longCaptureCanvas_;
    QImage longCaptureVisibleFrame_;
    QImage longCaptureViewportComposedFrame_;
    qint64 longCaptureViewportComposedFrameKey_ = -1;
    int longCaptureViewportComposedY_ = 0;
    QImage longCaptureResizePreviewBaseFrame_;
    QImage longCaptureResizePreviewFrame_;
    int longCaptureResizePreviewDocumentY_ = 0;
    LongCapture::RowSignature longCaptureDocSignature_;
    LongCapture::RowSignature longCaptureCurrentSignature_;
    LongCapture::RowSignature longCaptureLastSampleSignature_;
    QImage longCapturePreviewFitted_;
    QImage longCapturePreviewMip_;
    int longCapturePreviewMipShift_ = 0;
    int longCapturePreviewMipAnchorY_ = 0;
    int longCapturePreviewMipTopY_ = 0;
    int longCapturePreviewMipBottomY_ = 0;
    bool longCaptureActive_ = false;
    bool longCaptureInputRegionApplied_ = false;
    bool longCaptureCaptureBusy_ = false;
    bool longCaptureWheelDispatchInFlight_ = false;
    bool longCaptureWheelDispatchScheduled_ = false;
    bool longCaptureDispatchMovementObserved_ = false;
    bool longCaptureLastFrameRejected_ = false;
    bool longCapturePointerWheelLogged_ = false;
    bool longCaptureViewportStable_ = false;
    bool longCaptureAnnotationMode_ = false;
    bool longCaptureAnnotationModePending_ = false;
    bool longCaptureBrowseRevealPending_ = false;
    bool longCaptureAnnotationsOverlayRelative_ = false;
    bool longCaptureExclusionChecked_ = false;
    bool longCaptureExclusionAvailable_ = false;
    bool longCaptureViewportExclusionChecked_ = false;
    bool longCaptureViewportExclusionAvailable_ = false;
    int longCaptureViewportExclusionAttempts_ = 0;
    int longCaptureCanvasDocTopY_ = 0;
    int longCaptureDocTopY_ = 0;
    int longCaptureDocBottomY_ = 0;
    int longCaptureOutputTopY_ = 0;
    int longCaptureOutputBottomY_ = 0;
    int longCaptureCurrentY_ = 0;
    int longCapturePreviewPanelHeight_ = 0;
    int longCaptureExpectedDirection_ = 0;
    int longCaptureMotionDirection_ = 0;
    int longCaptureAnnotationStableSamples_ = 0;
    int longCaptureAnchorMismatchSamples_ = 0;
    int longCaptureAnchorMismatchY_ = 0;
    int longCaptureAnnotationConfirmRejects_ = 0;
    int longCaptureLastTrackedFrameMovement_ = 0;
    int longCapturePixelWheelRemainder_ = 0;
    int longCaptureDispatchedWheelDelta_ = 0;
    qint64 longCaptureLastWheelAtMs_ = -1;
    qint64 longCaptureLastMovementAtMs_ = -1;
    qint64 longCaptureWheelDispatchAtMs_ = -1;
    qint64 longCaptureLastPreviewRefreshAtMs_ = -1;
    qint64 longCapturePreviewDirtyAtMs_ = -1;
    quint64 longCaptureWheelReceivedCount_ = 0;
    quint64 longCaptureWheelDispatchedCount_ = 0;
    quint64 longCaptureWheelCompletedCount_ = 0;
#ifdef Q_OS_WIN
    std::unique_ptr<LongCaptureFrameGrabber> longCaptureFrameGrabber_;
#endif
    quint64 longCaptureFrameRequestSerial_ = 0;
    quint64 longCaptureActiveFrameRequestId_ = 0;
    LongCaptureFrameResume longCaptureActiveFrameResume_ = LongCaptureFrameResume::Synchronous;
    LongCaptureFrameResume longCaptureReadyFrameResume_ = LongCaptureFrameResume::Synchronous;
    QRect longCaptureActiveFrameGeometry_;
    QImage longCaptureReadyFrame_;
    LongCapture::RowSignature longCaptureReadyFrameSignature_;
    qint64 longCaptureReadyFrameCaptureMs_ = 0;
    bool longCaptureReadyFrameAvailable_ = false;
    QQueue<LongCaptureWheelSegment> longCaptureWheelQueue_;
    QString pendingLongCaptureAnnotationTool_;
    QPointF longCaptureAnnotationOverlayDelta_;
    quintptr longCaptureTargetHwnd_ = 0;
    QRect longCaptureAppliedRegionRect_;
    QRect longCaptureAppliedToolbarRegionRect_;
    QRect longCaptureAppliedPreviewRegionRect_;
    QRect longCaptureAppliedTopHandleRegionRect_;
    QRect longCaptureAppliedBottomHandleRegionRect_;
    QPoint longCaptureAppliedWindowGlobalTopLeft_;
    QSize longCaptureAppliedRegionWindowSize_;
    QPoint longCaptureAnchorGlobal_;
    QString longCaptureStatus_;
};

} // namespace Visnip
