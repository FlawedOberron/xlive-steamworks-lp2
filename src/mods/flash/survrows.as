// LP2 mods: Survival hides the stock Respawn row (Settings 3) of the Kills categories it shares,
// Elimination (1) and Team Elimination (5). The native side calls _root.lp2SurvivalRows with "1"
// while Survival is picked, "0" otherwise, and edits its own row table the same way.
function lp2SurvivalRows(on)
{
   var cats = [1,5];
   var i = 0;
   while(i < cats.length)
   {
      var rows = OnlineRule.sub2ItemArray[cats[i]];
      if(on == "1")
      {
         if(rows[1] == OnlineRule.ITEM_RESPORN)
         {
            rows.splice(1,1);
         }
      }
      else if(rows[1] != OnlineRule.ITEM_RESPORN)
      {
         rows.splice(1,0,OnlineRule.ITEM_RESPORN);
      }
      i++;
   }
}
