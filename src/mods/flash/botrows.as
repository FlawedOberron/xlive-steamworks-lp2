// LP2 mods: more AI Enemy Battle rows, cloned from Game Type (item 1): ours are items 2-8 (Survival, Respawns,
// Bots, Difficulty, Brain, Personality, Taunts). Runs after the game's own root script; the native side moves
// the cursor over the rows, fills the values through _root.lp2BotRow and lays the shown rows out in screen
// order (top to bottom, from one gap above Map) through _root.lp2BotOrder.
var lp2Bot;
var lp2Gap;
var lp2Top;
var lp2ArrowDy;
var lp2Last = 8;
function lp2BotRow(n, label, value)
{
   var m = lp2Bot["mc_member" + n];
   m.mc_txtSubItem.txt_subItem.text = label;
   m.txt_subMenu.text = value;
   lp2Bot.setLetter(m.mc_txtSubItem.txt_subItem,2);
   return m != undefined;
}
// order: the shown items, comma separated, top first ("4,0,1,5,6,7,8"); the rest hide
function lp2BotOrder(order)
{
   var items = order.split(",");
   var k = 2;
   while(k <= lp2Last)
   {
      lp2Bot["mc_member" + k]._visible = false;
      k++;
   }
   var i = 0;
   while(i < items.length)
   {
      var n = Number(items[i]);
      var m = lp2Bot["mc_member" + n];
      m._visible = true;
      m._y = lp2Top + lp2Gap * i;
      lp2Bot["mc_arrowL" + (n + 1)]._y = m._y + lp2ArrowDy;
      lp2Bot["mc_arrowR" + (n + 1)]._y = m._y + lp2ArrowDy;
      i++;
   }
   return true;
}
// The panel's rows are built after this frame's script on Scaleform: set up on the first frame they exist.
function lp2Setup()
{
   lp2Bot = mc_main.mc_BOT;
   lp2Gap = lp2Bot.mc_member1._y - lp2Bot.mc_member0._y;
   lp2Top = lp2Bot.mc_member0._y - lp2Gap;
   lp2ArrowDy = lp2Bot.mc_arrowL1._y - lp2Bot.mc_member0._y;
   var lp2N = 2;
   while(lp2N <= lp2Last)
   {
      lp2Bot.mc_member1.duplicateMovieClip("mc_member" + lp2N,900 + lp2N);
      lp2Bot.mc_arrowL2.duplicateMovieClip("mc_arrowL" + (lp2N + 1),910 + lp2N);
      lp2Bot.mc_arrowR2.duplicateMovieClip("mc_arrowR" + (lp2N + 1),920 + lp2N);
      lp2Bot["mc_arrowL" + (lp2N + 1)]._visible = false;
      lp2Bot["mc_arrowR" + (lp2N + 1)]._visible = false;
      lp2Bot["mc_member" + lp2N].mc_txtSubItem.gotoAndStop(IGame.getLangShortName());
      lp2Bot["mc_member" + lp2N].gotoAndStop("unfocus");
      itemBotArray.push(lp2Bot["mc_member" + lp2N]);
      IGame.setMouseAction(lp2Bot["mc_member" + lp2N],MouseId.BOT_MENU1 + lp2N);
      IGame.setMouseAction(lp2Bot["mc_arrowL" + (lp2N + 1)],GlobalMouseId.ARROW_L);
      IGame.setMouseAction(lp2Bot["mc_arrowR" + (lp2N + 1)],GlobalMouseId.ARROW_R);
      mc_main.botRotationArray.push(lp2N == 4 ? 144 : lp2N == 2 ? 216 : 240);
      lp2N++;
   }
}
_root.createEmptyMovieClip("lp2Wait",9990).onEnterFrame = function()
{
   if(mc_main.mc_BOT.mc_member1._y != undefined)
   {
      lp2Setup();
      delete this.onEnterFrame;
   }
};
var lp2SelectItem = selectItem;
selectItem = function(index)
{
   lp2SelectItem(index);
   if(phaseNo == PHASE_BOT)
   {
      var _loc1_ = 1;
      while(_loc1_ <= lp2Last + 1)
      {
         lp2Bot["mc_arrowL" + _loc1_]._visible = index == _loc1_ - 1;
         lp2Bot["mc_arrowR" + _loc1_]._visible = index == _loc1_ - 1;
         _loc1_++;
      }
   }
};
