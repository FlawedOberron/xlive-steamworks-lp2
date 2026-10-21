// LP2 mods: two more AI Enemy Battle rows (Survival, Respawns), cloned from Game Type.
// Runs after the game's own root script; the native side moves the cursor over 4 rows and
// fills the values through _root.lp2BotRows.
var lp2Bot = mc_main.mc_BOT;
var lp2Gap = lp2Bot.mc_member1._y - lp2Bot.mc_member0._y;
var lp2N = 2;
while(lp2N <= 3)
{
   lp2Bot.mc_member1.duplicateMovieClip("mc_member" + lp2N,900 + lp2N);
   lp2Bot.mc_arrowL2.duplicateMovieClip("mc_arrowL" + (lp2N + 1),910 + lp2N);
   lp2Bot.mc_arrowR2.duplicateMovieClip("mc_arrowR" + (lp2N + 1),920 + lp2N);
   lp2Bot["mc_member" + lp2N]._y = lp2Bot.mc_member1._y + lp2Gap * (lp2N - 1);
   lp2Bot["mc_arrowL" + (lp2N + 1)]._y = lp2Bot.mc_arrowL2._y + lp2Gap * (lp2N - 1);
   lp2Bot["mc_arrowR" + (lp2N + 1)]._y = lp2Bot.mc_arrowR2._y + lp2Gap * (lp2N - 1);
   lp2Bot["mc_arrowL" + (lp2N + 1)]._visible = false;
   lp2Bot["mc_arrowR" + (lp2N + 1)]._visible = false;
   lp2Bot["mc_member" + lp2N].mc_txtSubItem.gotoAndStop(IGame.getLangShortName());
   lp2Bot["mc_member" + lp2N].gotoAndStop("unfocus");
   itemBotArray.push(lp2Bot["mc_member" + lp2N]);
   IGame.setMouseAction(lp2Bot["mc_member" + lp2N],MouseId.BOT_MENU1 + lp2N);
   IGame.setMouseAction(lp2Bot["mc_arrowL" + (lp2N + 1)],GlobalMouseId.ARROW_L);
   IGame.setMouseAction(lp2Bot["mc_arrowR" + (lp2N + 1)],GlobalMouseId.ARROW_R);
   lp2N++;
}
mc_main.botRotationArray.push(216,240);
function lp2BotRows(label2, value2, label3, value3)
{
   lp2Bot.mc_member2.mc_txtSubItem.txt_subItem.text = label2;
   lp2Bot.mc_member2.txt_subMenu.text = value2;
   lp2Bot.mc_member3.mc_txtSubItem.txt_subItem.text = label3;
   lp2Bot.mc_member3.txt_subMenu.text = value3;
   lp2Bot.setLetter(lp2Bot.mc_member2.mc_txtSubItem.txt_subItem,2);
   lp2Bot.setLetter(lp2Bot.mc_member3.mc_txtSubItem.txt_subItem,2);
}
var lp2SelectItem = selectItem;
selectItem = function(index)
{
   lp2SelectItem(index);
   if(phaseNo == PHASE_BOT)
   {
      var _loc1_ = 1;
      while(_loc1_ <= 4)
      {
         lp2Bot["mc_arrowL" + _loc1_]._visible = index == _loc1_ - 1;
         lp2Bot["mc_arrowR" + _loc1_]._visible = index == _loc1_ - 1;
         _loc1_++;
      }
   }
};
