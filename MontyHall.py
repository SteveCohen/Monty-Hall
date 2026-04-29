import random
from decimal import Decimal

class montyHall:
	def __init__(self,rounds=500000):
		self.doors=[1,2,3] #Doors available in this simulation
		random.seed()
		self.results={'changed':{'won':0,'lost':0},'not':{'won':0,'lost':0}} #Store results here.
		print("Running Monty Hall simulation (%s rounds)..." % rounds)
		for i in range(0,rounds):
			self.run()

	def run(self,verbose=False):
		car=random.randint(1,3) #A car is behind a random door
		chosen=random.randint(1,3) #We initially choose a random door.
		if verbose: print ("Car: %s Chosen %s" % (car,chosen))

		#determine which door the host opens: not the chosen and not the car.
		hostOpen=list(self.doors) #Explicit copy so self.doors is never aliased or mutated.
		for d in self.doors:
			if (d==car) or (d==chosen): #Unless it's the car OR the one we chose.
				hostOpen=list(filter(lambda a: a != d, hostOpen))
		hostOpen=random.choice(hostOpen)
		if verbose: print ("Host chose %s" % hostOpen)

		#Player chooses again (randomly), from one of the remaining doors:
		playerChoice=list(filter(lambda a: a != hostOpen, self.doors))
		playerChoice=random.choice(playerChoice)

		#Record whether we changed or not, for purposes of scoring.
		if playerChoice!=chosen:
			if verbose: print("Player Changed")
			changeStatus='changed'
		else:
			if verbose: print("Player No Change")
			changeStatus='not'

		#Scoring
		if playerChoice==car:
			if verbose: print("WON!")
			self.results[changeStatus]['won']+=1
		else:
			if verbose: print("Lost")
			self.results[changeStatus]['lost']+=1
		if verbose: print("-------------")

	def printScores(self):
		for label,key in [('changing','changed'),('not changing','not')]:
			won=self.results[key]['won']
			lost=self.results[key]['lost']
			total=won+lost
			if total==0:
				print("When %s: no data recorded." % label)
			else:
				pct=Decimal(won)/Decimal(total)
				print("When %s, you win %.2f percent of the time." % (label,pct))

if __name__=="__main__":
	m=montyHall()
	print(m.results)
	m.printScores()
