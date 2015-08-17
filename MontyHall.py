import random
from decimal import *

class montyHall:
	def __init__(self,rounds=500000):
		self.doors=[1,2,3] #Doors available in this simulation
		random.seed()
		self.results={'changed':{'won':0,'lost':0},'not':{'won':0,'lost':0}} #Store results here.
		print("Running Monty Hall simulation (%s rounds)..." % rounds) 
		for i in range(0,rounds):
			self.run()
		
	def run(self,verbose=False):
		self.car=random.randint(1,3) #A car is behind a random door
		self.chosen=random.randint(1,3) #We initially choose a random door. 
		if verbose: print ("Car: %s Chosen %s" % (self.car,self.chosen) )

		#determine which door the host opens: not the chosen and not the car.
		self.hostOpen=self.doors #Host will choose to remove one door from all the doors available..		
		for d in self.doors:  
			if (d==self.car) or (d==self.chosen): #Unless it's the car OR the one we chose.
				self.hostOpen=filter(lambda a: a != d, self.hostOpen)
		self.hostOpen=random.choice(self.hostOpen)
		if verbose: print ("Host chose %s" % self.hostOpen)


		#Player chooses again (randomly), from one of the remaining doors:
		self.playerChoice=filter(lambda a: a != self.hostOpen, self.doors)
		self.playerChoice=random.choice(self.playerChoice)
		
		#Record whether we changed or not, for purposes of scoring..
		if self.playerChoice<>self.chosen:
			if verbose: print("Player Changed"),
			changeStatus='changed'
		else:
			if verbose: print("Player No Change"),
			changeStatus='not'
		
		#Scoring
		if self.playerChoice==self.car:
			if verbose: print("WON!")
			self.results[changeStatus]['won']+=1
		else: 
			if verbose: print("Lost")
			self.results[changeStatus]['lost']+=1		
		if verbose: print("-------------")
		
	def printScores(self):
		stats={'change':0.0,'not':0.0}
		stats['change']=Decimal(self.results['changed']['won'])/(Decimal(self.results['changed']['won'])+Decimal(self.results['changed']['lost']))
		stats['not']=Decimal(self.results['not']['won'])/(Decimal(self.results['not']['won'])+Decimal(self.results['not']['lost']))
		print("When changing, you win %.2f percent of the time. When not changing you win %.2f pct of the time" % (stats['change'],stats['not']))

if __name__=="__main__":
	m=montyHall()
	print(m.results)
	m.printScores()
		
