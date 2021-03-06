// This file is part of VSTGUI. It is subject to the license terms
// in the LICENSE file found in the top-level directory of this
// distribution and at http://github.com/steinbergmedia/vstgui/LICENSE

#include "vstgui/lib/animation/animations.h"
#include "vstgui/lib/animation/timingfunctions.h"
#include "vstgui/lib/cdatabrowser.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cvstguitimer.h"
#include "vstgui/lib/idatabrowserdelegate.h"
#include "vstgui/standalone/include/helpers/appdelegate.h"
#include "vstgui/standalone/include/helpers/menubuilder.h"
#include "vstgui/standalone/include/helpers/preferences.h"
#include "vstgui/standalone/include/helpers/uidesc/customization.h"
#include "vstgui/standalone/include/helpers/uidesc/modelbinding.h"
#include "vstgui/standalone/include/helpers/value.h"
#include "vstgui/standalone/include/helpers/windowcontroller.h"
#include "vstgui/standalone/include/helpers/windowlistener.h"
#include "vstgui/standalone/include/iapplication.h"
#include "vstgui/standalone/include/iuidescwindow.h"
#include "vstgui/uidescription/delegationcontroller.h"
#include "vstgui/uidescription/iuidescription.h"
#include "vstgui/uidescription/uiattributes.h"
#include <cassert>
#include <chrono>
#include <random>
#include <vector>

//------------------------------------------------------------------------
namespace VSTGUI {
namespace Standalone {
namespace Minesweeper {

//------------------------------------------------------------------------
class Model
{
private:
	enum class Bit : uint16_t
	{
		Mine = 1 << 0,
		Open = 1 << 1,
		Flag = 1 << 2,
		Trap = 1 << 3,
		Question = 1 << 4,
	};

	static bool hasBit (uint16_t s, Bit flag) { return (s & static_cast<uint16_t> (flag)) != 0; }
	static void setBit (uint16_t& s, Bit flag) { s |= static_cast<uint16_t> (flag); }
	static void unsetBit (uint16_t& s, Bit flag) { s &= ~static_cast<uint16_t> (flag); }

	struct Cell
	{
		uint16_t neighbours {0};
		uint16_t flags {0};

		bool isMine () const { return hasBit (flags, Bit::Mine); }
		bool isOpen () const { return hasBit (flags, Bit::Open); }
		bool isFlag () const { return hasBit (flags, Bit::Flag); }
		bool isTrap () const { return hasBit (flags, Bit::Trap); }
		bool isQuestion () const { return hasBit (flags, Bit::Question); }

		void setMine () { setBit (flags, Bit::Mine); }
		void setOpen () { setBit (flags, Bit::Open); }
		void setTrap () { setBit (flags, Bit::Trap); }
		void setFlag () { setBit (flags, Bit::Flag); }
		void unsetFlag () { unsetBit (flags, Bit::Flag); }
		void setQuestion () { setBit (flags, Bit::Question); }
		void unsetQuestion () { unsetBit (flags, Bit::Question); }
	};

	using Row = std::vector<Cell>;
	using Matrix = std::vector<Row>;

public:
	struct IListener
	{
		virtual void onCellChanged (uint32_t row, uint32_t col) = 0;
	};

	Model (uint32_t numberOfRows, uint32_t numberOfCols, uint32_t numberOfMines,
	       IListener* listener)
	: mines (numberOfMines), listener (listener)
	{
		assert (numberOfMines < numberOfRows * numberOfCols);
		allocateModel (numberOfRows, numberOfCols);
		clearModel ();
		setMines ();
		calcNeighbours ();
	}

	uint32_t getNumberOfMines () const { return mines; }
	uint32_t getNumberOfFlags () const { return flagged; }

	bool isTrapped () const { return trapped; }
	bool isDone () const
	{
		if (flagged == mines)
		{
			for (auto row = 0u; row < matrix.size (); ++row)
			{
				for (auto col = 0u; col < matrix.front ().size (); ++col)
				{
					auto cell = matrix[row][col];
					if (cell.isFlag () && !cell.isMine ())
						return false;
				}
			}
			return true;
		}
		return false;
	}

	void open (uint32_t row, uint32_t col)
	{
		if (openInternal (row, col))
		{
			auto cell = matrix[row][col];
			if (cell.isOpen () && cell.neighbours == 0)
			{
				openCleanNearby (row, col);
			}
		}
	}

	void mark (uint32_t row, uint32_t col)
	{
		auto& cell = matrix[row][col];
		if (cell.isFlag ())
		{
			--flagged;
			cell.unsetFlag ();
			cell.setQuestion ();
		}
		else if (cell.isQuestion ())
		{
			cell.unsetQuestion ();
		}
		else
		{
			++flagged;
			cell.setFlag ();
		}
		if (listener)
			listener->onCellChanged (row, col);
	}

	bool isOpen (uint32_t row, uint32_t col) const { return matrix[row][col].isOpen (); }
	bool isFlag (uint32_t row, uint32_t col) const { return matrix[row][col].isFlag (); }
	bool isQuestion (uint32_t row, uint32_t col) const { return matrix[row][col].isQuestion (); }
	bool isMine (uint32_t row, uint32_t col) const { return matrix[row][col].isMine (); }
	bool isTrapMine (uint32_t row, uint32_t col) const { return matrix[row][col].isTrap (); }
	uint32_t getNumberOfMinesNearby (uint32_t row, uint32_t col) const
	{
		return matrix[row][col].neighbours;
	}

private:
	void openCleanNearby (uint32_t row, uint32_t col)
	{
		const auto maxCol = matrix.front ().size () - 1;
		const auto maxRow = matrix.size () - 1;
		if (row > 0)
		{
			if (col > 0)
				open (row - 1, col - 1);
			open (row - 1, col);
			if (col < maxCol)
				open (row - 1, col + 1);
		}
		if (col > 0)
			open (row, col - 1);
		if (col < maxCol)
			open (row, col + 1);
		if (row < maxRow)
		{
			if (col > 0)
				open (row + 1, col - 1);
			open (row + 1, col);
			if (col < maxCol)
				open (row + 1, col + 1);
		}
	}

	bool openInternal (uint32_t row, uint32_t col)
	{
		auto& cell = matrix[row][col];
		if (!cell.isOpen ())
		{
			cell.setOpen ();
			++opened;
			if (cell.isFlag ())
			{
				cell.unsetFlag ();
				--flagged;
			}
			else if (cell.isQuestion ())
				cell.unsetQuestion ();
			if (listener)
				listener->onCellChanged (row, col);
			if (cell.isMine ())
			{
				cell.setTrap ();
				trapped = true;
				return false;
			}
			return true;
		}
		return false;
	}
	void allocateModel (uint32_t numberOfRows, uint32_t numberOfCols)
	{
		matrix.resize (numberOfRows);
		std::for_each (matrix.begin (), matrix.end (),
		               [numberOfCols] (auto& r) { r.resize (numberOfCols); });
	}
	void clearModel ()
	{
		std::for_each (matrix.begin (), matrix.end (), [] (auto& r) {
			std::for_each (r.begin (), r.end (), [] (auto& cell) { cell = {}; });
		});
	}
	void setMines ()
	{
		std::random_device rd;
		std::mt19937 gen (rd ());
		std::uniform_int_distribution<> rowDist (0, static_cast<int> (matrix.size () - 1));
		std::uniform_int_distribution<> colDist (0, static_cast<int> (matrix.front ().size () - 1));
		for (auto i = 0u; i < mines; ++i)
		{
			auto row = rowDist (gen);
			auto col = colDist (gen);
			if ((matrix[row][col]).isMine ())
			{
				assert (i > 0u);
				--i;
				continue;
			}
			matrix[row][col].setMine ();
		}
	}

	void calcNeighbours ()
	{
		const auto maxCol = matrix.front ().size () - 1;
		const auto maxRow = matrix.size () - 1;
		for (auto row = 0u; row < matrix.size (); ++row)
		{
			for (auto col = 0u; col < matrix.front ().size (); ++col)
			{
				auto& cell = matrix[row][col];
				if (cell.isMine ())
					continue;
				if (row > 0)
				{
					if (col > 0)
						cell.neighbours += matrix[row - 1][col - 1].isMine () ? 1 : 0;
					cell.neighbours += matrix[row - 1][col].isMine () ? 1 : 0;
					if (col < maxCol)
						cell.neighbours += matrix[row - 1][col + 1].isMine () ? 1 : 0;
				}
				if (col > 0)
					cell.neighbours += matrix[row][col - 1].isMine () ? 1 : 0;
				if (col < maxCol)
					cell.neighbours += matrix[row][col + 1].isMine () ? 1 : 0;
				if (row < maxRow)
				{
					if (col > 0)
						cell.neighbours += matrix[row + 1][col - 1].isMine () ? 1 : 0;
					cell.neighbours += matrix[row + 1][col].isMine () ? 1 : 0;
					if (col < maxCol)
						cell.neighbours += matrix[row + 1][col + 1].isMine () ? 1 : 0;
				}
			}
		}
	}

	Matrix matrix;
	uint32_t mines {0};
	uint32_t opened {0};
	uint32_t flagged {0};
	bool trapped {false};
	IListener* listener {nullptr};
};

using VSTGUI::DelegationController;
using VSTGUI::CDataBrowser;

static constexpr auto maxTimeInSeconds = 999;

//------------------------------------------------------------------------
class MinefieldViewController : public DelegationController,
                                public VSTGUI::DataBrowserDelegateAdapter,
                                public VSTGUI::NonAtomicReferenceCounted,
                                public VSTGUI::ViewListenerAdapter,
                                public Model::IListener
{
public:
	static constexpr auto BombCharacter = "\xF0\x9F\x92\xA3";
	static constexpr auto FlagCharacter = "\xF0\x9F\x9A\xA9";
	static constexpr auto ExplosionCharacter = "\xF0\x9F\x92\xA5";
	static constexpr auto QuestionMarkCharacter = "\xE2\x9D\x93";

	MinefieldViewController (IValue& flagsValue, IValue& timeValue, IController* parent)
	: DelegationController (parent), flagsValue (flagsValue), timeValue (timeValue)
	{
	}

	void startGame (uint32_t rows, uint32_t cols, uint32_t mines)
	{
		if (lostView)
		{
			lostView->removeAllAnimations ();
			lostView->setAlphaValue (0.f);
		}
		if (wonView)
		{
			wonView->removeAllAnimations ();
			wonView->setAlphaValue (0.f);
		}
		model = std::make_unique<Model> (rows, cols, mines, this);
		numRows = rows;
		numCols = cols;
		updateCellSize (dataBrowser->getViewSize ().getSize ());
		Value::performSinglePlainEdit (flagsValue, model->getNumberOfMines ());
		Value::performSinglePlainEdit (timeValue, 0);
		startTime = {};
	}

	void setMouseMode (bool state) { mouseMode = state; }

	CView* createView (const UIAttributes& attributes, const IUIDescription* description) override
	{
		const auto attr = attributes.getAttributeValue (VSTGUI::IUIDescription::kCustomViewName);
		if (attr && *attr == "MinefieldView")
		{
			description->getColor ("card.closed.frame", closedFrameColor);
			description->getColor ("card.closed.back", closedBackColor);
			description->getColor ("card.opened.frame", openedFrameColor);
			description->getColor ("card.opened.back", openedBackColor);
			description->getColor ("card.flaged.frame", flagedFrameColor);
			description->getColor ("card.flaged.back", flagedBackColor);
			if (auto f = description->getFont ("emoji"))
				emojiFont = *f;
			smallEmojiFont = emojiFont;
			if (dataBrowser)
				dataBrowser->unregisterViewListener (this);
			dataBrowser = nullptr;
			dataBrowser = new CDataBrowser ({}, this, 0, 0.);
			dataBrowser->registerViewListener (this);
			return dataBrowser;
		}
		return DelegationController::createView (attributes, description);
	}

	CView* verifyView (CView* view, const UIAttributes& attributes,
	                   const IUIDescription* description) override
	{
		const auto attr = attributes.getAttributeValue (VSTGUI::IUIDescription::kCustomViewName);
		if (attr)
		{
			if (*attr == "LostView")
			{
				lostView = view;
				lostView->setAlphaValue (0.f);
			}
			else if (*attr == "WonView")
			{
				wonView = view;
				wonView->setAlphaValue (0.f);
			}
		}

		return DelegationController::verifyView (view, attributes, description);
	}

	void onCellChanged (uint32_t row, uint32_t col) override
	{
		if (!dataBrowser)
			return;
		auto r = dataBrowser->getCellBounds (CDataBrowser::Cell (row, col));
		r.extend (1., 1.);
		dataBrowser->invalidRect (r);
	}

	int32_t dbGetNumRows (CDataBrowser* browser) override { return numRows; }
	int32_t dbGetNumColumns (CDataBrowser* browser) override { return numCols; }
	CCoord dbGetRowHeight (CDataBrowser* browser) override { return cellSize.y; }
	CCoord dbGetCurrentColumnWidth (int32_t index, CDataBrowser* browser) override
	{
		return cellSize.x;
	}
	bool dbGetLineWidthAndColor (CCoord& width, CColor& color, CDataBrowser* browser) override
	{
		width = 1;
		color = kBlackCColor;
		return true;
	}

	void drawClosedCell (const CRect& r, CDrawContext& context) const
	{
		context.setFrameColor (closedFrameColor);
		context.setFillColor (closedBackColor);
		context.drawRect (r, kDrawFilledAndStroked);
	}

	void drawOpenCell (const CRect& r, CDrawContext& context) const
	{
		context.setFrameColor (openedFrameColor);
		context.setFillColor (openedBackColor);
		context.drawRect (r, kDrawFilledAndStroked);
	}

	void drawQuestionMark (const CRect& r, CDrawContext& context, CFontRef f) const
	{
		context.setFont (f);
		context.setFontColor (kRedCColor);
		context.drawString (QuestionMarkCharacter, r);
	}

	void drawQuestionMarkCell (const CRect& r, CDrawContext& context, CFontRef f) const
	{
		context.setFrameColor (flagedFrameColor);
		context.setFillColor (flagedBackColor);
		context.drawRect (r, kDrawFilledAndStroked);
		drawQuestionMark (r, context, f);
	}

	void drawFlag (const CRect& r, CDrawContext& context, CFontRef f) const
	{
		context.setFont (f);
		context.setFontColor (kRedCColor);
		context.drawString (FlagCharacter, r);
	}

	void drawFlaggedCell (const CRect& r, CDrawContext& context, CFontRef f) const
	{
		context.setFrameColor (flagedFrameColor);
		context.setFillColor (flagedBackColor);
		context.drawRect (r, kDrawFilledAndStroked);
		drawFlag (r, context, f);
	}

	void drawMinedCell (const CRect& r, CDrawContext& context, CFontRef f) const
	{
		context.setFont (f);
		context.setFontColor (kBlackCColor);
		context.drawString (BombCharacter, r);
	}

	void drawExplosionCell (const CRect& r, CDrawContext& context, CFontRef f) const
	{
		context.setFont (f);
		context.setFontColor (kRedCColor);
		context.drawString (ExplosionCharacter, r);
	}

	void drawCellNeighbours (const CRect& r, CDrawContext& context, CFontRef f, uint32_t neighbours)
	{
		if (neighbours == 0)
			return;
		auto valueStr = toString (neighbours);
		context.setFont (f);
		context.setFontColor (kBlackCColor);
		context.drawString (valueStr, r);
	}

	void dbDrawCell (CDrawContext* context, const CRect& size, int32_t row, int32_t column,
	                 int32_t flags, CDataBrowser* browser) override
	{
		if (row < 0 || column < 0 || !model)
			return;
		context->setDrawMode (kAntiAliasing);
		context->setLineWidth (1.);
		CRect r (size);
		r.inset (1.5, 1.5);
		if (!model->isDone () && !model->isTrapped () && !model->isOpen (row, column))
		{
			if (model->isFlag (row, column))
			{
				drawFlaggedCell (r, *context, &emojiFont);
			}
			else if (model->isQuestion (row, column))
			{
				drawQuestionMarkCell (r, *context, &emojiFont);
			}
			else
			{
				drawClosedCell (r, *context);
			}
			return;
		}
		drawOpenCell (r, *context);
		if (model->isMine (row, column))
		{
			if (model->isTrapMine (row, column))
				drawExplosionCell (r, *context, &emojiFont);
			else
				drawMinedCell (r, *context, &emojiFont);
		}
		else
		{
			auto value = model->getNumberOfMinesNearby (row, column);
			drawCellNeighbours (r, *context, &font, value);
		}
		if (model->isFlag (row, column))
		{
			r.setWidth (r.getWidth () / 2.);
			r.setHeight (r.getHeight () / 2.);
			drawFlag (r, *context, &smallEmojiFont);
		}
	}

	CMouseEventResult dbOnMouseDown (const CPoint& where, const CButtonState& buttons, int32_t row,
	                                 int32_t column, CDataBrowser* browser) override
	{
		ignoreMouseUp = false;
		if (!mouseMode && buttons.isLeftButton ())
		{
			mouseDownTimer = makeOwned<CVSTGUITimer> (
			    [this, row, column] (auto) {
				    mouseDownTimer = nullptr;
				    if (!model->isOpen (row, column))
				    {
					    model->mark (row, column);
					    checkGameOver ();
				    }
				    ignoreMouseUp = true;
			    },
			    60);
		}
		return kMouseEventHandled;
	}

	CMouseEventResult dbOnMouseMoved (const CPoint& where, const CButtonState& buttons, int32_t row,
	                                  int32_t column, CDataBrowser* browser) override
	{
		return kMouseEventHandled;
	}

	CMouseEventResult dbOnMouseUp (const CPoint& where, const CButtonState& buttons, int32_t row,
	                               int32_t column, CDataBrowser* browser) override
	{
		mouseDownTimer = nullptr;
		if (ignoreMouseUp || !model || row < 0 || column < 0 || model->isTrapped () ||
		    model->isDone ())
			return kMouseEventHandled;
		if (!model->isOpen (row, column))
		{
			if (buttons.isRightButton ())
			{
				model->mark (row, column);
			}
			else if (buttons.isLeftButton ())
			{
				if (!model->isFlag (row, column) && !model->isQuestion (row, column))
					model->open (row, column);
				else
					model->mark (row, column);
			}
			checkGameOver ();
		}
		return kMouseEventHandled;
	}

	void checkGameOver ()
	{
		if (startTime == TimePoint {})
		{
			startTime = Clock::now ();
			gameTimer = makeOwned<CVSTGUITimer> ([this] (auto) { onTimer (); }, 1000);
		}
		Value::performSinglePlainEdit (flagsValue,
		                               model->getNumberOfMines () - model->getNumberOfFlags ());
		if (model->isDone () && !model->isTrapped ())
		{
			wonView->addAnimation ("Won", new VSTGUI::Animation::AlphaValueAnimation (1.f),
			                       new VSTGUI::Animation::RepeatTimingFunction (
			                           new VSTGUI::Animation::LinearTimingFunction (250), -1));
			gameTimer = nullptr;
			dataBrowser->invalid ();
		}
		if (model->isTrapped ())
		{
			lostView->addAnimation ("Lost", new VSTGUI::Animation::AlphaValueAnimation (1.f),
			                        new VSTGUI::Animation::RepeatTimingFunction (
			                            new VSTGUI::Animation::LinearTimingFunction (100), -1));
			gameTimer = nullptr;
			dataBrowser->invalid ();
		}
	}

	void onTimer ()
	{
		auto now = Clock::now ();
		auto distance = std::chrono::duration_cast<std::chrono::seconds> (now - startTime).count ();
		if (distance >= maxTimeInSeconds)
			gameTimer = nullptr;
		Value::performSinglePlainEdit (timeValue, static_cast<IValue::Type> (distance));
	}

	void viewSizeChanged (CView* view, const CRect& oldSize) override
	{
		if (!dataBrowser)
			return;
		auto newSize = view->getViewSize ().getSize ();
		updateCellSize (newSize);
	}

	void updateCellSize (CPoint newSize)
	{
		newSize -= {3., 3.};
		cellSize.x = newSize.x / numCols;
		cellSize.y = newSize.y / numRows;
		dataBrowser->recalculateLayout ();
		font.setSize (cellSize.y / 2.);
		emojiFont.setSize (cellSize.y / 2.);
		smallEmojiFont.setSize (font.getSize () / 2.);
	}

private:
	SharedPointer<CDataBrowser> dataBrowser;
	SharedPointer<CView> lostView;
	SharedPointer<CView> wonView;
	int32_t numRows {1};
	int32_t numCols {1};
	std::unique_ptr<Model> model;
	CColor closedFrameColor {kBlackCColor};
	CColor closedBackColor {kGreyCColor};
	CColor openedFrameColor {kGreyCColor};
	CColor openedBackColor {kTransparentCColor};
	CColor flagedFrameColor {kGreyCColor};
	CColor flagedBackColor {kTransparentCColor};
	CPoint cellSize {30, 30};
	CFontDesc font {*kSystemFont};
	CFontDesc smallEmojiFont {*kSymbolFont};
	CFontDesc emojiFont {*kSymbolFont};
	IValue& flagsValue;
	IValue& timeValue;

	using Clock = std::chrono::steady_clock;
	using TimePoint = std::chrono::time_point<Clock>;
	TimePoint startTime;
	SharedPointer<CVSTGUITimer> gameTimer;
	SharedPointer<CVSTGUITimer> mouseDownTimer;
	bool ignoreMouseUp {false};
	bool mouseMode {true};
};

//------------------------------------------------------------------------
class DigitsDisplayConverter : public IValueConverter
{
public:
	DigitsDisplayConverter (ValueConverterPtr converter, uint32_t digits)
	: converter (converter), digits (digits)
	{
	}

	UTF8String valueAsString (IValue::Type value) const override
	{
		auto plain = static_cast<uint32_t> (std::round (converter->normalizedToPlain (value)));
		UTF8String str;
		auto val = static_cast<uint32_t> (std::pow (10, digits - 1));
		for (auto i = digits; i > 0; --i)
		{
			auto v = plain / val;
			str += toString (v);
			plain -= v * val;
			val /= 10;
		}
		return str;
	}
	IValue::Type stringAsValue (const UTF8String& string) const override
	{
		return converter->stringAsValue (string);
	}
	IValue::Type plainToNormalized (IValue::Type plain) const override
	{
		return converter->plainToNormalized (plain);
	}
	IValue::Type normalizedToPlain (IValue::Type normalized) const override
	{
		return converter->normalizedToPlain (normalized);
	}

private:
	ValueConverterPtr converter;
	uint32_t digits {3};
};

static constexpr IdStringPtr GameGroup = "Game";
static const Command NewGameCommand {GameGroup, "New Game"};
static const Command NewBeginnerGameCommand {GameGroup, "New Beginner Game"};
static const Command NewIntermediateGameCommand {GameGroup, "New Intermediate Game"};
static const Command NewExpertGameCommand {GameGroup, "New Expert Game"};

static constexpr IdStringPtr MouseMode = "Use Mouse Mode";
static constexpr IdStringPtr TouchpadMode = "Use Touchpad Mode";
static const Command MouseModeCommand {GameGroup, MouseMode};
static const Command TouchpadModeCommand {GameGroup, TouchpadMode};

//------------------------------------------------------------------------
class WindowController : public WindowControllerAdapter,
                         public UIDesc::Customization,
                         public UIDesc::IModelBinding,
                         public ICommandHandler
{
public:
	static constexpr auto valueRows = "Rows";
	static constexpr auto valueCols = "Cols";
	static constexpr auto valueMines = "Mines";
	static constexpr auto valueStart = "Start";
	static constexpr auto valueFlags = "Flags";
	static constexpr auto valueTime = "Time";
	static constexpr auto valueMouseMode = "MouseMode";

	WindowController ()
	{
		IApplication::instance ().registerCommand (NewGameCommand, 'n');
		IApplication::instance ().registerCommand (NewBeginnerGameCommand, 0);
		IApplication::instance ().registerCommand (NewIntermediateGameCommand, 0);
		IApplication::instance ().registerCommand (NewExpertGameCommand, 0);
		IApplication::instance ().registerCommand (MouseModeCommand, 0);
		IApplication::instance ().registerCommand (TouchpadModeCommand, 0);

		addCreateViewControllerFunc (
		    "MinefieldController", [this] (const auto& name, auto* parent, auto* uidesc) {
			    if (!minefieldViewController)
			    {
				    auto flagsValue = modelBinding.getValue (valueFlags);
				    auto timeValue = modelBinding.getValue (valueTime);
				    minefieldViewController =
				        new MinefieldViewController (*flagsValue, *timeValue, parent);
					if (auto valueObject = modelBinding.getValue (valueMouseMode))
					{
					    minefieldViewController->setMouseMode (
					        valueObject->getValue () >= 0.5 ? true : false);
				    }
			    }
			    minefieldViewController->remember ();
			    return minefieldViewController;
		    });
		modelBinding.addValue (
		    Value::make (valueRows, 0, Value::makeRangeConverter (8, 30, 0)),
		    UIDesc::ValueCalls::onEndEdit ([this] (auto& value) { verifyNumMines (); }));
		modelBinding.addValue (
		    Value::make (valueCols, 0, Value::makeRangeConverter (8, 30, 0)),
		    UIDesc::ValueCalls::onEndEdit ([this] (auto& value) { verifyNumMines (); }));
		modelBinding.addValue (
		    Value::make (valueMines, 0, Value::makeRangeConverter (4, 668, 0)),
		    UIDesc::ValueCalls::onEndEdit ([this] (auto& value) { verifyNumMines (); }));
		modelBinding.addValue (Value::make (
		    valueFlags, 0,
		    std::make_shared<DigitsDisplayConverter> (Value::makeRangeConverter (0, 668, 0), 2)));
		modelBinding.addValue (
		    Value::make (valueTime, 0,
		                 std::make_shared<DigitsDisplayConverter> (
		                     Value::makeRangeConverter (0, maxTimeInSeconds, 0), 3)));
		if (auto value = modelBinding.getValue (valueMines))
			Value::performSinglePlainEdit (*value.get (), 10);
		modelBinding.addValue (
		    Value::make (valueStart),
		    UIDesc::ValueCalls::onEndEdit ([this] (auto& value) { startNewGame (); }));
		modelBinding.addValue (
		    Value::make (valueMouseMode), UIDesc::ValueCalls::onPerformEdit ([this] (auto& value) {
			    if (minefieldViewController)
				    minefieldViewController->setMouseMode (value.getValue () >= 0.5 ? true : false);
		    }));

		loadDefaults ();
	}

	void loadDefaults ()
	{
		Preferences prefs ("Values");
		if (auto value = prefs.getNumber<int32_t> (valueRows))
		{
			if (auto valueObject = modelBinding.getValue (valueRows))
				Value::performSinglePlainEdit (*valueObject, *value);
		}
		if (auto value = prefs.getNumber<int32_t> (valueCols))
		{
			if (auto valueObject = modelBinding.getValue (valueCols))
				Value::performSinglePlainEdit (*valueObject, *value);
		}
		if (auto value = prefs.getNumber<int32_t> (valueMines))
		{
			if (auto valueObject = modelBinding.getValue (valueMines))
				Value::performSinglePlainEdit (*valueObject, *value);
		}
		if (auto value = prefs.getNumber<bool> (valueMouseMode))
		{
			if (auto valueObject = modelBinding.getValue (valueMouseMode))
				Value::performSinglePlainEdit (*valueObject, *value);
		}
	}

	void storeDefaults ()
	{
		Preferences prefs ("Values");
		if (auto value = modelBinding.getValue (valueRows))
			prefs.setNumber (valueRows, static_cast<int32_t> (Value::currentPlainValue (*value)));
		if (auto value = modelBinding.getValue (valueCols))
			prefs.setNumber (valueCols, static_cast<int32_t> (Value::currentPlainValue (*value)));
		if (auto value = modelBinding.getValue (valueMines))
			prefs.setNumber (valueMines, static_cast<int32_t> (Value::currentPlainValue (*value)));
		if (auto value = modelBinding.getValue (valueMouseMode))
			prefs.setNumber (valueMouseMode, static_cast<bool> (Value::currentPlainValue (*value)));
	}

	bool canHandleCommand (const Command& command) override
	{
		if (command.group == GameGroup)
		{
			if (command.name == TouchpadMode)
			{
				if (modelBinding.getValue (valueMouseMode)->getValue () < 0.5)
					return false;
			}
			if (command.name == MouseMode)
			{
				if (modelBinding.getValue (valueMouseMode)->getValue () >= 0.5)
					return false;
			}
			return true;
		}
		return false;
	}

	bool handleCommand (const Command& command) override
	{
		if (command.group != GameGroup)
			return false;
		if (command.name == TouchpadMode)
		{
			auto value = modelBinding.getValue (valueMouseMode);
			Value::performSingleEdit (*value, 0.);
			return true;
		}
		else if (command.name == MouseMode)
		{
			auto value = modelBinding.getValue (valueMouseMode);
			Value::performSingleEdit (*value, 1.);
			return true;
		}

		auto rowValue = modelBinding.getValue (valueRows);
		auto colValue = modelBinding.getValue (valueCols);
		auto mineValue = modelBinding.getValue (valueMines);
		if (!rowValue || !colValue || !mineValue)
			return false;
		if (command == NewBeginnerGameCommand)
		{
			Value::performSinglePlainEdit (*rowValue, 9);
			Value::performSinglePlainEdit (*colValue, 9);
			Value::performSinglePlainEdit (*mineValue, 10);
		}
		else if (command == NewIntermediateGameCommand)
		{
			Value::performSinglePlainEdit (*rowValue, 16);
			Value::performSinglePlainEdit (*colValue, 16);
			Value::performSinglePlainEdit (*mineValue, 40);
		}
		else if (command == NewExpertGameCommand)
		{
			Value::performSinglePlainEdit (*rowValue, 16);
			Value::performSinglePlainEdit (*colValue, 30);
			Value::performSinglePlainEdit (*mineValue, 99);
		}
		startNewGame ();
		return true;
	}

	void beforeShow (IWindow& w) override { window = &w; }
	void onShow (const IWindow& w) override { startNewGame (); }
	void onClosed (const IWindow& w) override { storeDefaults (); }

	const ValueList& getValues () const override { return modelBinding.getValues (); }

	void startNewGame ()
	{
		assert (minefieldViewController);
		auto rows =
		    static_cast<uint32_t> (Value::currentPlainValue (*modelBinding.getValue (valueRows)));
		auto cols =
		    static_cast<uint32_t> (Value::currentPlainValue (*modelBinding.getValue (valueCols)));
		auto mines =
		    static_cast<uint32_t> (Value::currentPlainValue (*modelBinding.getValue (valueMines)));
		minefieldViewController->startGame (rows, cols, mines);
	}

	void verifyNumMines ()
	{
		auto rows = Value::currentPlainValue (*modelBinding.getValue (valueRows));
		auto cols = Value::currentPlainValue (*modelBinding.getValue (valueCols));
		auto mines = Value::currentPlainValue (*modelBinding.getValue (valueMines));
		if (rows * cols < mines)
		{
			Value::performSinglePlainEdit (*modelBinding.getValue (valueMines), rows * cols * 0.8);
		}
	}

private:
	UIDesc::ModelBindingCallbacks modelBinding;
	MinefieldViewController* minefieldViewController {nullptr};
	IWindow* window {nullptr};
};

//------------------------------------------------------------------------
class MyApplication : public Application::DelegateAdapter,
                      public WindowListenerAdapter,
                      public MenuBuilderAdapter
{
public:
	MyApplication ()
	: Application::DelegateAdapter ({"Minesweeper", "1.0.0", "vstgui.examples.minesweeper"})
	{
	}

	void finishLaunching () override
	{
		auto windowController = std::make_shared<WindowController> ();
		UIDesc::Config config;
		config.uiDescFileName = "Window.uidesc";
		config.viewName = "Window";
		config.customization = windowController;
		config.modelBinding = windowController;
		config.windowConfig.title = "Minesweeper";
		config.windowConfig.autoSaveFrameName = "MinesweeperWindow";
		config.windowConfig.style.border ().close ().centered ().size ();
		if (auto window = UIDesc::makeWindow (config))
		{
			window->show ();
			window->registerWindowListener (this);
		}
		else
		{
			IApplication::instance ().quit ();
		}
	}
	void onClosed (const IWindow& window) override { IApplication::instance ().quit (); }

	SortFunction getCommandGroupSortFunction (const Interface& context,
	                                          const UTF8String& group) const override
	{
		if (group == GameGroup)
		{
			return [] (const UTF8String& lhs, const UTF8String& rhs) {
				static auto order = {NewGameCommand.name, NewBeginnerGameCommand.name,
				                     NewIntermediateGameCommand.name, NewExpertGameCommand.name};
				auto leftIndex = std::find (order.begin (), order.end (), lhs);
				auto rightIndex = std::find (order.begin (), order.end (), rhs);
				return std::distance (leftIndex, rightIndex) > 0;
			};
		}
		return {};
	}

	bool prependMenuSeparator (const Interface& context, const Command& cmd) const override
	{
		if (cmd == Commands::CloseWindow || cmd == MouseModeCommand)
			return true;
		return false;
	}
};

static Application::Init gAppDelegate (std::make_unique<MyApplication> ());

//------------------------------------------------------------------------
} // Minesweeper
} // Standalone
} // VSTGUI
