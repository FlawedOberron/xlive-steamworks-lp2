// LP2 mods: the mod build on the main menu (flash\mainmenu\id010_360), small, bottom right above the
// bottom bar. The DLL writes the text over the placeholder (same length, space padded) when it adds this.
var lp2Build = "@@LP2BUILD@@                                                    ";
function lp2BuildShow()
{
   var s = lp2Build;
   while(s.length > 0 && s.charAt(s.length - 1) == " ")
   {
      s = s.substr(0,s.length - 1);
   }
   _root.createTextField("lp2BuildTxt",9992,780,640,480,24);
   var t = _root.lp2BuildTxt;
   var f = _root.mc_CINESCO.txt_Msg.getTextFormat();
   f.size = 14;
   f.align = "right";
   t.embedFonts = true;
   t.selectable = false;
   t.setNewTextFormat(f);
   t.text = s;
   t.textColor = 12303291;
   if(t.textWidth == 0)
   {
      t.embedFonts = false; // no embedded glyphs for it: the device font
      t.text = s;
   }
}
lp2BuildShow();
