#include <cmath>
#include <fstream>
#include <utility>
#include <unordered_map>
#include <boost/math/special_functions/gamma.hpp>
#include <boost/random.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/numeric.hpp>
#include <boost/timer.hpp>
#include "HdpLda.h"
#include <omp.h>


using namespace hdplda;

HdpLda::HdpLda(const Corpus &corpus, const Vocabulary &vocabulary, const unsigned long seed
			 , const double &gamma, const double &alpha0, const double &beta, const int &K
			 , const double &gamma_a, const double &gamma_b, const double &alpha0_a, const double &alpha0_b)
			 : corpus(corpus), vocabulary(vocabulary), gamma(gamma), alpha0(alpha0), beta(beta)
			 , D(corpus.D), V(corpus.V), N(corpus.N), K(K), gamma_a(gamma_a), gamma_b(gamma_b), alpha0_a(alpha0_a), alpha0_b(alpha0_b)
{
	m = 0;
	engine.seed(seed);
	
	restaurants.resize(D);

	topics.push_back(shared_ptr<Topic>(new Topic(V, beta)));
	shared_ptr<Topic> &firstTopic = *(topics.begin());

	for(int j=0; j<D; ++j){
		Restaurant &restaurant = restaurants[j];
		const Document &document = corpus.documents[j];

		restaurant.tables.push_back(shared_ptr<Table>(new Table(firstTopic)));
		shared_ptr<Table> &firstTable = *(restaurant.tables.begin());
		firstTopic->m++;
		m++;

		restaurant.n = document.tokens.size();
		restaurant.customers.resize(restaurant.n);
		for(int i=0; i<restaurant.n; ++i){
			int v = document.tokens[i].x;
			restaurant.customers[i].word = v;
			firstTable->n++;
			firstTable->n_v[v]++;
			firstTable->topic->n++;
			firstTable->topic->n_v[v]++;
			restaurant.customers[i].table = firstTable;
		}
	}
}



void HdpLda::sampling(void)
{
	sampleTables();
	sampleTopics();
	K = topics.size();
}



void HdpLda::sampleTables(void)
{
	using namespace boost;
	using std::shared_ptr;

	uniform_real<> uniformDistribution(0, 1);
	variate_generator<mt19937&, uniform_real<>> uniform(engine, uniformDistribution);

	for(int j=0; j<D; ++j){
		Restaurant &restaurant = restaurants[j];
		vector<Customer> &customers = restaurant.customers;
		list<shared_ptr<Table>> &tables = restaurant.tables;

		for(int i=0; i<customers.size(); ++i){
			int v = customers[i].word;
			customers[i].table->n--;
			customers[i].table->n_v[v]--;
			customers[i].table->topic->n--;
			customers[i].table->topic->n_v[v]--;
			int &dbg = customers[i].table->topic->n;
			// 客がいなくなった場合はそのテーブルを削除
			if(customers[i].table->n == 0){
				shared_ptr<Topic> &topicOfThisTable = customers[i].table->topic;
				tables.remove(customers[i].table);
				topicOfThisTable->m--;
				m--;
				// 削除されたテーブルに乗っていた料理が、全レストランのどのテーブルにも提供されていなくなった場合はその料理をフランチャイズのメニューから削除
				if(topicOfThisTable->m == 0){
					topics.remove(topicOfThisTable);
				}
			}

			// テーブルごとの着席確率の離散累積分布を求める（ただし正規化されていない着席確率）
			int T = tables.size();
			int K = topics.size();
			vector<double> unnormalizedCDF(T + 1);
			vector<shared_ptr<Table>> ptrTables(T);
			double sum = 0.0;
			
			// テーブルサンプリング式(式24)の第二項目のG_0(v)を求める
			// ついでにそのとき計算するψ_jt (== φ_k)をキャッシュしておく
			// また、新しいテーブルが選ばれたときのためにk ∝ G0_v(k)の(正規化されていない)離散累積分布も求めておく
			
			double G0_v = 0.0;
			vector<double> phi_kv(T);
			vector<double> G0_vk(K+1);
			vector<shared_ptr<Topic>> ptrTopics(K);
			{
				int k = 0;
				for(auto topic=topics.begin(); topic!=topics.end(); ++topic){
					(*topic)->phi_v[v] = ((*topic)->n_v[v] + beta) / ((*topic)->n + V * beta); // φ_k(v)
					G0_v += (*topic)->m * (*topic)->phi_v[v]; //m_.k * φ_k(v)
					G0_vk[k] = G0_v;
					ptrTopics[k] = (*topic);
					k++;
				}
				G0_v += gamma * (1.0 / static_cast<double>(V)); // γH(v), because H(v) = β/Vβ = 1/V
				G0_vk[K] = G0_v;
				G0_v /= (m + gamma);
			}

			// テーブルサンプリング式(式24)の第一項目の分子を求める（分母は必要ない）
			{
				int t = 0;
				for(auto table = tables.begin(); table!=tables.end(); ++table){
					shared_ptr<Topic> &topic = (*table)->topic;
					sum += (*table)->n * topic->phi_v[v]; // n_jt. * ψ_jt(v)
					unnormalizedCDF[t] = sum;
					ptrTables[t] = (*table);
					++t;
				}
			}

			sum += alpha0 * G0_v; // 新しいテーブルに着く確率
			unnormalizedCDF[T] = sum;
			
			// 離散累積分布からテーブル番号をサンプリング
			double tRnd = uniform() * unnormalizedCDF[T];
			int tNew = T;
			for(int t=0; t<T+1; ++t){
				if(unnormalizedCDF[t] > tRnd){
					tNew = t;
					break;
				}
			}

			// 着席するテーブルを更新
			shared_ptr<Table> newTable;
			if(tNew < T){ // 既存のテーブルの場合
				newTable = ptrTables[tNew];
			}
			else{ // 新しいテーブルの場合
				// 新しいテーブルに乗せる料理（トピック）のサンプリング
				double kRnd = uniform() * G0_vk[K];
				int kNew = K;
				for(int k=0; k<K+1; ++k){
					if(G0_vk[k] > kRnd){
						kNew = k;
						//for(int b=0; b<=K; ++b){cout << G0_vk[b] << " ";}
						//cout <<"(" <<kRnd<<"), G0_v = "<<G0_v<< endl;
						break;
					}
				}
				shared_ptr<Topic> newTopic;
				if(kNew < K){ // 既存のトピックの場合
					newTopic = ptrTopics[kNew];
				}
				else{ // 新しいトピックの場合
					newTopic = shared_ptr<Topic>(new Topic(V, beta));
					topics.push_back(newTopic);
				}
				newTable = shared_ptr<Table>(new Table(newTopic));
				tables.push_back(newTable);
				newTopic->m++;
				m++;
			}
			newTable->n++;
			newTable->n_v[v]++;
			newTable->topic->n++;
			newTable->topic->n_v[v]++;
			customers[i].table = newTable;
		}
	}
}

// log(gamma(x + n) / gamma(x)) == log(x * (x+1) * (x+2) * ... * (x+n-1))
inline double HdpLda::logRisingFactorial(const double &x, const double &n)
{
	using namespace boost::math;
	
	if(n > 13){ // nが約13より大きいときはlgammaを使ったほうが速い
		if(x < 1.0e+15){
			return lgamma(x + n) - lgamma(x);
		}
	}
	else{ // 上昇階乗を直接計算したもののlog（最速）
		if(x < 1.0e+22){
			double total = 1.0;
			for(int i=0; i<n; ++i){
				total *= (x + i);
			}
			return log(total);
		}
	}
	// (n>13 && x>=1.0e+15) or (n<=13 && x>=1.0e+22)のときはlogの和（遅い）
	double total = 0.0;
	for(int i=0; i<n; ++i){
		total += log(x + i);
	}
	return total;
}

void HdpLda::sampleTopics(void)
{
	using namespace boost;
	using std::shared_ptr;
	timer timer;
	double tm = 0.0;
	uniform_real<> uniformDistribution(0, 1);
	variate_generator<mt19937&, uniform_real<>> uniform(engine, uniformDistribution);
	
	for(int j=0; j<D; ++j){
		Restaurant &restaurant = restaurants[j];
		vector<Customer> &customers = restaurant.customers;
		list<shared_ptr<Table>> &tables = restaurant.tables;
		
		timer.restart();
		for(auto it=tables.begin(); it!=tables.end(); ++it){
			shared_ptr<Table> &table = *it;
			shared_ptr<Topic> &oldTopic = table->topic;
			m--;
			oldTopic->m--;

			// このテーブルに着いている単語とそのカウントを求めておく
			vector<pair<int, int>> n_v; // n_v.first = v, n_v.second = table->n_v[v]
			for(int v=0; v<V; ++v){
				if(table->n_v[v] != 0){
					n_v.push_back(pair<int, int>(v, table->n_v[v]));
				}
			}
			// 料理が提供されているテーブルがなくなったらメニューから料理を削除
			if(oldTopic->m == 0){
				topics.remove(oldTopic);
			}
			else{	
				oldTopic->n -= table->n;
				for(int l=0; l<n_v.size(); ++l){
					int v = n_v[l].first;
					int count = n_v[l].second;
					oldTopic->n_v[v] -= count;
				}
			}
			
			// 料理ごとの選択確率を求める
			// ただし、値が非常に小さくなるので対数で計算する
			int K = topics.size();
			vector<double> unnormalizedCDF(K + 1);
			vector<double> logPk(K + 1, 0.0);
			vector<shared_ptr<Topic>> ptrTopics(K);
			boost::copy(topics, ptrTopics.begin());

#pragma omp for
			for(int k=0; k<K; ++k){
				shared_ptr<Topic> &topic = ptrTopics[k];

				logPk[k] = log(static_cast<double>(topic->m));
				logPk[k] -= logRisingFactorial(topic->n + V * beta, table->n);
				for(int l=0; l<n_v.size(); ++l){
					logPk[k] += logRisingFactorial(topic->n_v[n_v[l].first] + beta, n_v[l].second); 
				}
			}
			// 新しい料理をサンプリングする確率の対数
			logPk[K] = log(gamma);
			logPk[K] -= logRisingFactorial(V * beta, table->n);
			for(int l=0; l<n_v.size(); ++l){
				logPk[K] += logRisingFactorial(beta, n_v[l].second);
			}
			
			double maxLogP = *(boost::min_element(logPk));
			unnormalizedCDF[0] = exp(logPk[0] - maxLogP);
			for(int k=1; k<K+1; ++k){
				unnormalizedCDF[k] = unnormalizedCDF[k-1] + exp(logPk[k] - maxLogP);
			}

			// 離散累積分布からテーブル番号をサンプリング
			double kRnd = uniform() * unnormalizedCDF[K];
			int kNew = K;
			for(int k=0; k<K+1; ++k){
				if(unnormalizedCDF[k] > kRnd){
					kNew = k;
					break;
				}
			}

			shared_ptr<Topic> newTopic;
			if(kNew < K){ // 既存のトピックの場合
				newTopic = ptrTopics[kNew];
			}
			else{ // 新しいトピックの場合
				newTopic = shared_ptr<Topic>(new Topic(V, beta));
				topics.push_back(newTopic);
			}
			newTopic->m++;
			newTopic->n += table->n;
			for(int l=0; l<n_v.size(); ++l){
				int v = n_v[l].first;
				int count = n_v[l].second;
				newTopic->n_v[v] += count;
			}
			table->topic = newTopic;
			m++;
		}
		tm += timer.elapsed();
	}
//	cout << "\ttm="<<tm<<endl;
}



double HdpLda::calcPerplexity(const vector<vector<double>> &phi, const vector<vector<double>> &theta)
{
	double perplexity = 0.0;
	const int K = phi.size();

	for(int j=0; j<D; ++j){
		vector<Customer> &customers = restaurants[j].customers;
		for(int i=0; i<customers.size(); ++i){
			int v = customers[i].word;
			double p_v = 0.0;
			for(int k=0; k<K; ++k){
				p_v += theta[j][k] * phi[k][v];
			}
			perplexity -= log(p_v);
		}
	}

	perplexity = exp(perplexity / static_cast<double>(N));

	return perplexity;
}



vector<vector<double>> HdpLda::calcPhi(void)
{
	
	const int K = topics.size();
	vector<vector<double>> phi(K);

	boost::for_each(phi, [&](vector<double> &phi_k){phi_k.resize(V, 0.0);});

	vector<shared_ptr<Topic>> ptrTopics(K);
	boost::copy(topics, ptrTopics.begin());

	// Φを求める
	for(int k=0; k<K; ++k){
		shared_ptr<Topic> &topic = ptrTopics[k];
		for(int v=0; v<V; ++v){
			phi[k][v] = (topic->n_v[v] + beta) / (topic->n + V * beta);
		}
	}

	return phi;
}



vector<vector<double>> HdpLda::calcTheta(void)
{
	double perplexity = 0.0;
	const int K = topics.size();
	vector<vector<double>> theta(D);
	boost::for_each(theta, [&](vector<double> &theta_j){theta_j.resize(K, 0.0);});

	vector<shared_ptr<Topic>> ptrTopics(K);
	boost::copy(topics, ptrTopics.begin());

	for(int j=0; j<D; ++j){
		const int T = restaurants[j].tables.size();
		vector<shared_ptr<Table>> tables(T);		
		boost::copy(restaurants[j].tables, tables.begin());
		
		for(int t=0; t<T; ++t){
			int k = distance(ptrTopics.begin(), boost::find(ptrTopics, tables[t]->topic));
			theta[j][k] += tables[t]->n;
		}
		for(int k=0; k<K; ++k){
			theta[j][k] += alpha0 * (ptrTopics[k]->m + gamma / static_cast<double>(K)) / (m + gamma);
			theta[j][k] /= (restaurants[j].n + alpha0);
		}
	}

	return theta;
}



void HdpLda::savePhi(const vector<vector<double>> &phi, const string &fileName)
{	
	const int K = phi.size();
	ofstream ofs(fileName.c_str());
	for(int k=0; k<K; ++k){
		ofs << "Topic: " << k << endl;
		vector<pair<double, string>> phi_k;
		phi_k.resize(V);
		for(int v=0; v<V; ++v){
			phi_k[v].first = phi[k][v];
			phi_k[v].second = vocabulary.words[v].str;
		}

		boost::sort(phi_k, greater<pair<double, string>>());
			
		for(int v=0; v<20; ++v){
			ofs << "\t" << phi_k[v].second << ": " << phi_k[v].first << endl;
		}
		ofs << "\n" << endl;
	}
	ofs.close();
}



void HdpLda::saveTheta(const vector<vector<double>> &theta, const string &fileName)
{
	const int K = theta[0].size();
	ofstream ofs(fileName.c_str());
	for(int j=0; j<D; ++j){
		ofs << "Document: " << j << endl;
		vector<pair<double, int>> theta_j;
		theta_j.resize(K);
		for(int k=0; k<K; ++k){
			theta_j[k].first = theta[j][k];
			theta_j[k].second = k;
		}

		boost::sort(theta_j, greater<pair<double, int>>());
			
		for(int k=0; k<K; ++k){
			ofs << "\t" << theta_j[k].second << ": " << theta_j[k].first << endl;
		}
		ofs << "\n" << endl;
	}
	ofs.close();
}



void HdpLda::savePhiTheta(const vector<vector<double>> &phi, const string &phiFileName,
					const vector<vector<double>> &theta, const string &thetaFileName)
{
	savePhi(phi, phiFileName);
	saveTheta(theta, thetaFileName);
}

void HdpLda::showAllCounts(void)
{
	cout << "hdp.m: " << m << endl;
	cout << "hdp.topics.size(): " << topics.size() << endl;	
	cout << "hdp.topics[0].n: " << boost::accumulate(topics, 0, [](int n, shared_ptr<Topic> t){return n + t->n;}) << endl;	
}

void HdpLda::showAllParameters(void)
{
	cout << "gamma: " << gamma << ", alpha_0: " << alpha0 << ", beta: " << beta << endl;
}


double HdpLda::betaRandom(const double &alpha, const double &beta)
{
	double x = boost::gamma_distribution<>(alpha, 1.0)(engine);
	double y = boost::gamma_distribution<>(beta, 1.0)(engine);
	return x / (x + y);
}

// Escobar and West, "Bayesian Density Estimation and Inference Using Mixtures"
void HdpLda::sampleGamma(void)
{
	using namespace boost;

	double eta = betaRandom(gamma + 1.0, m); // 式(14)
	int k = topics.size();
	double p_pi = gamma_a + k - 1;
	p_pi = p_pi / (p_pi + m * (gamma_b - log(eta)));

	bool pi = bernoulli_distribution<>(p_pi)(engine);
	double shape = pi ? (gamma_a + k) : (gamma_a + k - 1);
	double scale = 1.0 / (gamma_b - log(eta));
	gamma = gamma_distribution<>(shape, scale)(engine); // 式(13)
}

		

void HdpLda::sampleAlpha0(const int &iter)
{
	using namespace boost;

	double sum_log_w;
	double sum_s;

	for(int i=0; i<iter; ++i){
		sum_log_w = 0.0;
		sum_s = 0.0;
		for(int j=0; j<D; ++j){
			const int &n_j = restaurants[j].n;
			sum_log_w += log(betaRandom(alpha0 + 1, n_j));
			sum_s += bernoulli_distribution<>(static_cast<double>(n_j) / (alpha0 + n_j))(engine); // bernoulli(n/α_0 + n)からサンプリング
		}
		double shape = alpha0_a + m - sum_s;
		double scale = 1.0 / (alpha0_b - sum_log_w); // bはrateパラメータ
		alpha0 = gamma_distribution<>(shape, scale)(engine);
	}
}



vector<double> HdpLda::calcSticksOfG0(void){
	const int K = topics.size();
	vector<double> stickLengths(K+1);
	boost::transform(topics, stickLengths.begin(), [](shared_ptr<Topic> topic){
		return topic->n;
	});
	stickLengths[K] = gamma;
	double total = boost::accumulate(stickLengths, 0.0);
	boost::for_each(stickLengths, [total](double &len){ len /= total;});

	return stickLengths;
}

vector<double> HdpLda::calcEntropyOfTopics(const vector<vector<double>> &phi)
{
	const int K = phi.size();
	vector<double> entropy_k(K);
	for(int k=0; k<K; ++k){
		entropy_k[k] = boost::accumulate(phi[k], 0.0, [](double sum, double p_v){ return sum -= (p_v * log(p_v));});
	}

	return entropy_k;
}