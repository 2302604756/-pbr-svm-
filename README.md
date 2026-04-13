孩子不懂事写着玩的
4/13
1.重写了地形的noise函数看着更好看一点  
2.写了简化的水体透视效果  
使用了水体与杂质分离的方法利用水体与杂质吸收系数Extinction不同  
通过浑浊度turbidity来控制透射率transmittance实现finalcolor的能量守恒计算  
FinalColor = waterColor*(1-transmittance)+underWaterColor*transmittance  
3.做了贴图混合效果，通过高度，坡度等来控制草地，水体，雪地，岩石贴图的混合  
4.新加了小demo动画(还很简陋)  

https://github.com/2302604756/-pbr-svm-/raw/master/pbr%E5%B0%8F%E5%9E%8Bdemo.mp4
