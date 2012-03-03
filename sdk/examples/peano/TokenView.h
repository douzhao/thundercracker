#pragma once

#include "sifteo.h"
#include "View.h"
#include "TotalsCube.h"
#include "ObjectPool.h"
#include "Game.h"

namespace TotalsGame {
  
	class Token;
	class IExpression;

  class TokenView : public View 
  {
  public:
      void OnShakeStarted(TotalsCube *c) ;
      
      void OnButtonEvent(TotalsCube *c, bool isPressed);

      
  private:
	  static int sHintParity;

      class EventHandler: public TotalsCube::EventHandler
      {
          TokenView *owner;
      public:
          EventHandler(TokenView *_owner): owner(_owner) {}
          void OnCubeTouch(TotalsCube *c, bool isPressed) {owner->OnButtonEvent(c, isPressed);}
          void OnCubeShake(TotalsCube *c) {owner->OnShakeStarted(c);}
      };
	  EventHandler eventHandler;

  public:

	static const Vec2 Mid;

    static const int BIT_TOP = 1<<SIDE_TOP;
    static const int BIT_LEFT = 1<<SIDE_LEFT;
    static const int BIT_BOTTOM = 1<<SIDE_BOTTOM;
    static const int BIT_RIGHT = 1<<SIDE_RIGHT;
    static const char *kOpNames[4];
    
	enum Status 
	{ 
		StatusIdle, 
		StatusQueued, 
		StatusOverlay, 
		StatusHinting 
	};

    Token *token;
    Status mStatus;
    IExpression *mCurrentExpression;    
    float mTimeout;
    int mDigitId;
    bool useAccentDigit;
    int renderedDigit;
    int mHideMask;

    Status GetCurrentStatus();
    bool mLit;

    //-------------------------------------------------------------------------
    // PUBLIC METHODS
    //-------------------------------------------------------------------------

    TokenView(TotalsCube *cube, Token *_token, bool showDigitOnInit=true);
	virtual ~TokenView() {}

    void HideOps() ;

    void PaintRandomNumeral() ;

    void ResetNumeral() ;

    void WillJoinGroup() ;

    void DidJoinGroup() ;
    void DidGroupDisconnect();

    void BlinkOverlay();

    void ShowOverlay() ;

    void ShowLit() ;

    void HideOverlay();

    void SetHideMode(int mask);

    //for placement new
    void* operator new (size_t size, void* ptr) throw() {return ptr;}
    void operator delete(void *ptr) {}

private:
    bool NotHiding(Cube::Side side) ;

    void SetState(Status state, bool resetTimer=true, bool resetExpr=true);


    //-------------------------------------------------------------------------
    // VIEW METHODS
    //-------------------------------------------------------------------------
public:
    virtual void DidAttachToCube(TotalsCube *c) ;
    
    virtual void WillDetachFromCube(TotalsCube *c) ;

    virtual void Update() ;

    //virtual void Paint() ;
    void PaintNow();
  
    //-------------------------------------------------------------------------
    // HELPER METHODS
    //-------------------------------------------------------------------------
	private:

      void PaintCenterCap(uint8_t masks[4]);
      static int CountBits(uint8_t mask);

      void PaintDigit();


    // // // //
    // code generated by ImageTool
    void PaintTop(bool lit);
    void PaintLeft(bool lit);
    void PaintRight(bool lit);
    void PaintBottom(bool lit);
    // // // //

   
  };
  
}

