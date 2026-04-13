[Transient Analysis]
{
   Npanes: 2
   {
      traces: 1 {303104003,0,"I(C1)"}
      Parametric: "V(source)-V(vout)"
      X: (' ',0,0.1,9.99,100)
      Y[0]: ('_',0,1,1,10)
      Y[1]: (' ',1,1e+308,0.1,-1e+308)
      Amps: ('_',1,1,1,1,1,10)
      Log: 1 1 0
      GridStyle: 1
      StepLegend: (0.115729508518907,9.36329208823942)
   },
   {
      traces: 2 {34668547,0,"I(C1)"} {524290,0,"100W/(V(source)-V(vout))"}
      Parametric: "V(source)-V(vout)"
      X: (' ',0,0.1,9.99,100)
      Y[0]: ('_',0,1,1,10)
      Y[1]: (' ',1,1e+308,0.1,-1e+308)
      Amps: ('_',1,0,0,1,1,10)
      Log: 1 1 0
      GridStyle: 1
      Text: "A" 2 (19.6359247687931,8.75882891004876) ;SOA Limit
   }
}
