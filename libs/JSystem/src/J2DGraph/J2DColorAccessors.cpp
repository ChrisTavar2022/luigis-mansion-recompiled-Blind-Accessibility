// Out-of-line J2DWindow/J2DTextBox color accessors. In Dusklight these live in
// TP game code (d_pane_class.cpp); Luigi's Mansion needs them in J2DGraph itself.
#include <JSystem/J2DGraph/J2DTextBox.h>
#include <JSystem/J2DGraph/J2DWindow.h>

JUtility::TColor J2DWindow::getBlack() const { return mBlack; }
JUtility::TColor J2DWindow::getWhite() const { return mWhite; }

bool J2DWindow::setBlack(JUtility::TColor black) {
    mBlack = black;
    return true;
}

bool J2DWindow::setWhite(JUtility::TColor white) {
    mWhite = white;
    return true;
}

bool J2DWindow::setBlackWhite(JUtility::TColor black, JUtility::TColor white) {
    mBlack = black;
    mWhite = white;
    return true;
}

bool J2DTextBox::setBlack(JUtility::TColor black) {
    mBlackColor = black;
    return true;
}
