#include <JSystem/JUtility/JUTFont.hpp>

JUTFont::JUTFont()
    : mColor1()
    , mColor2()
    , mColor3()
    , mColor4()
{
	setCharColor(JUtility::TColor());
	mFixed      = false;
	mFixedWidth = 0;
}

void JUTFont::initiate() { }

void JUTFont::setCharColor(JUtility::TColor col1)
{
	mColor1 = col1;
	mColor2 = col1;
	mColor3 = col1;
	mColor4 = col1;
}

void JUTFont::setGradColor(JUtility::TColor col1, JUtility::TColor col2)
{
	mColor1 = col1;
	mColor2 = col1;
	mColor3 = col2;
	mColor4 = col2;
}
