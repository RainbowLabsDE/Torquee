[Transient Analysis]
{
   Npanes: 1
   {
      traces: 5 {524290,2,"V(VIN,Source)*Id(M1)+V(Gate,Source)*Ig(M1)"} {524292,2,"V(VOUT,Source)*Id(M2)+V(Gate,Source)*Ig(M2)"} {524291,0,"V(source)-V(vout)"} {524293,0,"V(source)-V(gate)"} {34603014,1,"I(C1)"}
      X: ('m',0,0,0.03,0.35)
      Y[0]: (' ',0,0,5,60)
      Y[1]: (' ',1,0,0.3,3.6)
      Volts: (' ',0,0,0,0,5,60)
      Amps: (' ',0,0,1,0,0.3,3.6)
      Units: "W" (' ',0,0,0,0,7,70)
      Log: 0 0 0
      GridStyle: 1
   }
}
